#include "factory/server.hpp"

#include "factory/audit.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>

#include <httplib.h>

namespace factory {

namespace fs = std::filesystem;

namespace {
std::string read_file(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    if (!in) throw std::runtime_error("cannot read " + p.string());
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

fs::path find_resource_dir(const char* argv0, const std::string& name) {
    std::vector<fs::path> roots;
    if (const char* env = std::getenv("FACTORY_SIM_HOME")) roots.emplace_back(env);
    std::error_code ec;
    const fs::path exe_dir = fs::absolute(fs::path(argv0), ec).parent_path();
    roots.push_back(exe_dir);
    roots.push_back(exe_dir / "..");
    roots.push_back(exe_dir / ".." / "..");
    roots.push_back(fs::current_path());
    roots.push_back(fs::current_path() / "..");
    for (const auto& r : roots) {
        const fs::path candidate = r / name;
        if (fs::is_directory(candidate, ec)) return fs::weakly_canonical(candidate, ec);
    }
    return {};
}

json execute_run(const RunRequest& req, const NodeRegistry& registry) {
    BuildOptions opt;
    opt.trace = req.trace;
    auto plant = build_plant(req.config, registry, opt);
    plant->run();
    json out;
    out["summary"] = to_json(summarize(*plant));
    if (req.trace) out["trace"] = trace_to_json(*plant);
    if (req.whatif) out["whatif"] = to_json(what_if(req.config, registry, req.whatif_reps));
    if (req.replications > 1) out["replication"] = to_json(replicate(req.config, registry, req.replications));
    return out;
}

std::string make_standalone_html(const fs::path& web_dir, const json& payload) {
    std::string html = read_file(web_dir / "studio.html");
    std::string data = payload.dump();
    // Keep "</script>" inside the JSON from terminating the embedding script tag.
    for (std::size_t pos = 0; (pos = data.find("</", pos)) != std::string::npos; pos += 3) data.replace(pos, 2, "<\\/");
    const std::string marker = "/*__EMBEDDED_DATA__*/";
    const auto at = html.find(marker);
    if (at == std::string::npos) throw std::runtime_error("studio.html has no embedding marker");
    html.replace(at, marker.size(), "window.EMBEDDED = " + data + ";");
    return html;
}

int serve(const NodeRegistry& registry, const ServerOptions& opt) {
    if (opt.web_dir.empty()) {
        std::cerr << "error: could not find the web/ directory (set FACTORY_SIM_HOME or run from the project folder)\n";
        return 2;
    }
    httplib::Server svr;

    auto error_json = [](httplib::Response& res, int status, const std::string& msg) {
        res.status = status;
        res.set_content(json{{"error", msg}}.dump(), "application/json");
    };

    svr.Get("/", [&](const httplib::Request&, httplib::Response& res) {
        try {
            res.set_header("Cache-Control", "no-store");  // always serve the current UI
            res.set_content(read_file(opt.web_dir / "studio.html"), "text/html; charset=utf-8");
        } catch (const std::exception& e) {
            error_json(res, 500, e.what());
        }
    });

    svr.Get("/api/types", [&](const httplib::Request&, httplib::Response& res) {
        res.set_content(registry.describe().dump(), "application/json");
    });

    svr.Get("/api/examples", [&](const httplib::Request&, httplib::Response& res) {
        json list = json::array();
        std::error_code ec;
        std::vector<fs::path> files;
        if (!opt.examples_dir.empty())
            for (const auto& e : fs::directory_iterator(opt.examples_dir, ec))
                if (e.path().extension() == ".json") files.push_back(e.path());
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            try {
                json cfg = json::parse(read_file(f));
                list.push_back({{"name", f.stem().string()}, {"config", cfg}});
            } catch (const std::exception&) { /* skip unreadable examples */ }
        }
        res.set_content(list.dump(), "application/json");
    });

    auto parse_request = [](const httplib::Request& req) {
        json body = json::parse(req.body);
        RunRequest r;
        if (!body.contains("config")) throw ConfigError("request needs a 'config' field");
        r.config = body["config"];
        r.trace = body.value("trace", true);
        r.whatif = body.value("whatif", false);
        r.whatif_reps = body.value("whatif_reps", 3);
        r.replications = body.value("replications", 0);
        return r;
    };

    svr.Post("/api/run", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            res.set_content(execute_run(parse_request(req), registry).dump(), "application/json");
        } catch (const ConfigError& e) {
            error_json(res, 400, e.what());
        } catch (const json::exception& e) {
            error_json(res, 400, std::string("bad JSON: ") + e.what());
        } catch (const std::exception& e) {
            error_json(res, 500, e.what());
        }
    });

    svr.Post("/api/normalize", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            res.set_content(normalize_config(json::parse(req.body)).dump(), "application/json");
        } catch (const ConfigError& e) {
            error_json(res, 400, e.what());
        } catch (const json::exception& e) {
            error_json(res, 400, std::string("bad JSON: ") + e.what());
        }
    });

    svr.Post("/api/audit", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            const json body = json::parse(req.body);
            if (!body.contains("config")) throw ConfigError("request needs a 'config' field");
            AuditOptions opt;
            opt.replications = std::clamp(body.value("reps", 10), 2, 40);
            const AuditReport report = audit(body["config"], registry, opt);
            res.set_header("Content-Disposition", "attachment; filename=\"pipeline_audit.pdf\"");
            res.set_header("X-Audit-Score", std::to_string(report.score));
            res.set_content(render_pdf(report), "application/pdf");
        } catch (const ConfigError& e) {
            error_json(res, 400, e.what());
        } catch (const json::exception& e) {
            error_json(res, 400, std::string("bad JSON: ") + e.what());
        } catch (const std::exception& e) {
            error_json(res, 500, e.what());
        }
    });

    svr.Post("/api/export", [&](const httplib::Request& req, httplib::Response& res) {
        try {
            RunRequest r = parse_request(req);
            json payload = execute_run(r, registry);
            payload["config"] = r.config;
            payload["types"] = registry.describe();
            res.set_header("Content-Disposition", "attachment; filename=\"factory_replay.html\"");
            res.set_content(make_standalone_html(opt.web_dir, payload), "text/html; charset=utf-8");
        } catch (const ConfigError& e) {
            error_json(res, 400, e.what());
        } catch (const std::exception& e) {
            error_json(res, 500, e.what());
        }
    });

    const std::string url = "http://127.0.0.1:" + std::to_string(opt.port) + "/";
    if (!svr.bind_to_port("127.0.0.1", opt.port)) {
        std::cerr << "error: cannot listen on port " << opt.port << " (already in use?)\n";
        return 2;
    }
    std::cout << "Factory Studio running at " << url << "   (Ctrl+C to stop)\n" << std::flush;
    if (opt.open_browser) {
#ifdef _WIN32
        std::system(("start \"\" " + url).c_str());
#elif defined(__APPLE__)
        std::system(("open " + url).c_str());
#else
        std::system(("xdg-open " + url + " >/dev/null 2>&1 &").c_str());
#endif
    }
    svr.listen_after_bind();
    return 0;
}

}  // namespace factory
