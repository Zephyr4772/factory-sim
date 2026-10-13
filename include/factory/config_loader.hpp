#pragma once
// JSON -> Plant. Validates everything it can up front and reports errors with the
// node id and parameter name, so a bad config fails loudly instead of simulating nonsense.
//
// Config shape (all node parameters may also sit directly on the node object):
//   { "simulation": {duration_seconds, random_seed, warmup_seconds, sample_interval, stop_on_deadlock},
//     "node_types": { "cnc": {"base": "machine", "params": {...}} },      // your own reusable node types
//     "nodes": [ {"id", "type", "label", "x", "y", "params": {...},
//                 "routing": {"policy", "rules": [{"match": {...}, "to": [...]}]},
//                 "downstream": [ "id" | {"to", "weight", "rework", "bypass", "label"} ]} ] }
// The original station-based layout ("ingestion" + "stations") is converted automatically.
#include <memory>
#include <optional>

#include "factory/plant.hpp"
#include "factory/registry.hpp"

namespace factory {

struct BuildOptions {
    std::optional<std::uint64_t> seed;
    std::optional<double> duration_s;
    bool trace = false;
};

// Converts the original {ingestion, stations} format into the node graph format.
[[nodiscard]] json normalize_config(const json& config);

// Throws ConfigError on any problem. Warnings (e.g. possible deadlock cycles) are stored
// in Plant::warnings().
[[nodiscard]] std::unique_ptr<Plant> build_plant(const json& config, const NodeRegistry& registry,
                                                 const BuildOptions& options = {});

}  // namespace factory
