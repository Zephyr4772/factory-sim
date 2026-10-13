#include <fstream>
#include <iostream>
#include <iomanip>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "factory/analytics.hpp"
#include "factory/audit.hpp"
#include "factory/server.hpp"

using namespace factory;

namespace {

constexpr const char* kUsage = R"(factory_sim: discrete-event factory & pipeline sandbox

Usage:
  factory_sim run <config.json> [options]     simulate and print the results table
      --seed N          override simulation.random_seed
      --duration S      override simulation.duration_seconds
      --whatif          measure which improvement recovers the most throughput
      --reps K          K independent replications -> mean +- 95% CI
      --csv FILE        write WIP/throughput time series
      --json FILE       write the full results as JSON
      --html FILE       write a self-contained interactive replay page
  factory_sim audit <config.json> [options]   audit + health report of a pipeline as a PDF
      --pdf FILE        output file (default pipeline_audit.pdf)
      --reps K          replications used for the robustness checks (default 10)
      --json FILE       also write the findings as JSON
      --fail-below N    exit with code 5 if the health score is below N (for CI)
  factory_sim validate <config.json>          check a config without simulating
  factory_sim types                           list node types and their parameters
  factory_sim serve [--port N] [--open]       start Factory Studio (visual editor) in your browser
)";

std::string read_text(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw ConfigError("cannot open '" + path + "'");
    std::ostringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

json load_config(const std::string& path) {
    try {
        return json::parse(read_text(path));
    } catch (const json::parse_error& e) {
        throw ConfigError("'" + path + "' is not valid JSON: " + e.what());
    }
}

void write_text(const std::string& path, const std::string& text) {
    std::ofstream out(path, std::ios::binary);
    if (!out) throw ConfigError("cannot write '" + path + "'");
    out << text;
}

struct Args {
    std::vector<std::string> positional;
    std::map<std::string, std::string> opts;
    bool flag(const std::string& k) const { return opts.count(k) > 0; }
    std::string get(const std::string& k, const std::string& def = "") const {
        auto it = opts.find(k);
        return it == opts.end() ? def : it->second;
    }
};

Args parse_args(int argc, char** argv, int first) {
    static const std::set<std::string> valued = {"--seed", "--duration", "--reps", "--csv", "--json", "--html", "--port", "--pdf", "--fail-below"};
    Args a;
    for (int i = first; i < argc; ++i) {
        const std::string s = argv[i];
        if (s.rfind("--", 0) == 0) {
            if (valued.count(s)) {
                if (i + 1 >= argc) throw ConfigError("option " + s + " needs a value");
                a.opts[s] = argv[++i];
            } else {
                a.opts[s] = "1";
            }
        } else {
            a.positional.push_back(s);
        }
    }
    return a;
}

void print_whatif(const WhatIfReport& w) {
    std::cout << "\nWHAT-IF (measured, " << w.reps << " seeds, common random numbers)  baseline "
              << std::fixed << std::setprecision(1) << w.base_uph << " units/h\n";
    for (const auto& r : w.results)
        std::cout << "  " << std::left << std::setw(14) << r.node << std::setw(24) << r.change << std::right << std::setw(9)
                  << r.uph << " u/h   " << std::showpos << std::setw(7) << r.delta << " (" << std::setw(6) << r.delta_pct
                  << "%)" << std::noshowpos << "\n";
    std::cout << "  -> " << w.recommendation << "\n";
}

void print_replication(const ReplicationReport& r) {
    auto line = [](const char* name, const Stat& s, const char* unit) {
        std::cout << "  " << std::left << std::setw(16) << name << std::right << std::fixed << std::setprecision(2)
                  << std::setw(10) << s.mean << " +- " << std::setw(7) << s.ci95 << " " << unit << "   (sd " << s.sd << ")\n";
    };
    std::cout << "\nREPLICATIONS n=" << r.n << "  mean +- 95% confidence interval\n";
    line("throughput", r.uph, "units/h");
    line("avg WIP", r.wip, "units");
    line("cycle time", r.cycle_time_s, "s");
    line("lost arrivals", r.lost_pct, "%");
    if (r.deadlocks) std::cout << "  deadlocked runs: " << r.deadlocks << " of " << r.n << "\n";
}

int cmd_run(const Args& a, const NodeRegistry& reg, const char* argv0) {
    if (a.positional.empty()) throw ConfigError("run: missing <config.json>");
    const json cfg = load_config(a.positional[0]);
    BuildOptions opt;
    if (a.flag("--seed")) opt.seed = std::stoull(a.get("--seed"));
    if (a.flag("--duration")) opt.duration_s = std::stod(a.get("--duration"));
    opt.trace = a.flag("--html");
    auto plant = build_plant(cfg, reg, opt);
    plant->run();
    const Summary summary = summarize(*plant);
    std::cout << to_ascii(summary);
    if (a.flag("--csv")) write_text(a.get("--csv"), samples_csv(*plant));

    json payload;
    payload["summary"] = to_json(summary);
    if (a.flag("--whatif")) {
        const WhatIfReport w = what_if(cfg, reg, 3);
        print_whatif(w);
        payload["whatif"] = to_json(w);
    }
    if (a.flag("--reps")) {
        const ReplicationReport r = replicate(cfg, reg, std::stoi(a.get("--reps")));
        print_replication(r);
        payload["replication"] = to_json(r);
    }
    if (a.flag("--json")) write_text(a.get("--json"), payload.dump(2));
    if (a.flag("--html")) {
        const auto web = find_resource_dir(argv0, "web");
        if (web.empty()) throw ConfigError("cannot find the web/ directory for --html");
        payload["trace"] = trace_to_json(*plant);
        payload["config"] = cfg;
        payload["types"] = reg.describe();
        write_text(a.get("--html"), make_standalone_html(web, payload));
        std::cout << "wrote interactive replay: " << a.get("--html") << "\n";
    }
    return summary.deadlock ? 3 : 0;
}

int cmd_audit(const Args& a, const NodeRegistry& reg) {
    if (a.positional.empty()) throw ConfigError("audit: missing <config.json>");
    AuditOptions opt;
    opt.replications = std::stoi(a.get("--reps", "10"));
    const AuditReport report = audit(load_config(a.positional[0]), reg, opt);
    std::cout << audit_text_summary(report);
    const std::string pdf_path = a.get("--pdf", "pipeline_audit.pdf");
    write_text(pdf_path, render_pdf(report));
    std::cout << "wrote audit report: " << pdf_path << "\n";
    if (a.flag("--json")) write_text(a.get("--json"), to_json(report).dump(2));
    if (a.flag("--fail-below") && report.score < std::stoi(a.get("--fail-below"))) return 5;
    return 0;
}

int cmd_validate(const Args& a, const NodeRegistry& reg) {
    if (a.positional.empty()) throw ConfigError("validate: missing <config.json>");
    auto plant = build_plant(load_config(a.positional[0]), reg);
    std::cout << "OK: " << plant->node_count() << " nodes\n";
    for (const auto& w : plant->warnings()) std::cout << "warning: " << w << "\n";
    return 0;
}

int cmd_types(const NodeRegistry& reg) {
    for (const auto& t : reg.types()) {
        std::cout << t.name << "  [" << t.category << "]\n  " << t.description << "\n";
        for (const auto& p : t.params)
            std::cout << "    " << std::left << std::setw(16) << p.name << std::setw(13) << p.kind << p.doc << "\n";
        std::cout << "\n";
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string(argv[1]) == "help" || std::string(argv[1]) == "--help") {
        std::cout << kUsage;
        return argc < 2 ? 1 : 0;
    }
    const std::string cmd = argv[1];
    try {
        const NodeRegistry reg = NodeRegistry::with_defaults();
        const Args a = parse_args(argc, argv, 2);
        if (cmd == "run") return cmd_run(a, reg, argv[0]);
        if (cmd == "audit") return cmd_audit(a, reg);
        if (cmd == "validate") return cmd_validate(a, reg);
        if (cmd == "types") return cmd_types(reg);
        if (cmd == "serve") {
            ServerOptions so;
            so.port = std::stoi(a.get("--port", "8080"));
            so.open_browser = a.flag("--open");
            so.web_dir = find_resource_dir(argv[0], "web");
            so.examples_dir = find_resource_dir(argv[0], "examples");
            return serve(reg, so);
        }
        std::cerr << "unknown command '" << cmd << "'\n\n" << kUsage;
        return 1;
    } catch (const ConfigError& e) {
        std::cerr << "error: " << e.what() << "\n";
        return 2;
    } catch (const std::exception& e) {
        std::cerr << "fatal: " << e.what() << "\n";
        return 4;
    }
}
