#pragma once
// Local HTTP server behind the Studio UI, plus the standalone-HTML exporter.
#include <filesystem>
#include <string>

#include "factory/analytics.hpp"

namespace factory {

struct ServerOptions {
    int port = 8080;
    bool open_browser = false;
    std::filesystem::path web_dir;
    std::filesystem::path examples_dir;
};

// Locates web/ and examples/ relative to the executable or the working directory.
[[nodiscard]] std::filesystem::path find_resource_dir(const char* argv0, const std::string& name);

// Runs one config and returns {summary, trace?, whatif?, replication?} exactly as /api/run does.
struct RunRequest {
    json config;
    bool trace = true;
    bool whatif = false;
    int whatif_reps = 3;
    int replications = 0;
};
[[nodiscard]] json execute_run(const RunRequest& req, const NodeRegistry& registry);

// Studio page with the results embedded so it works offline from a single file.
[[nodiscard]] std::string make_standalone_html(const std::filesystem::path& web_dir, const json& payload);

int serve(const NodeRegistry& registry, const ServerOptions& options);  // blocks

}  // namespace factory
