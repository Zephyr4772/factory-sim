#pragma once
// The node-type registry: maps a "type" string in a config to a factory plus a parameter
// schema. The schema powers config validation, `factory_sim types`, and the Studio's
// auto-generated property forms, so a newly registered node type appears everywhere.
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "factory/node.hpp"
#include "factory/params.hpp"
#include "factory/plant.hpp"

namespace factory {

struct ParamSpec {
    std::string name;
    std::string kind;  // number | integer | boolean | string | distribution | object | enum
    std::string doc;
    json def;                          // default shown in the UI (null = none)
    std::vector<std::string> choices;  // for kind == "enum"
};

struct NodeTypeInfo {
    std::string name;
    std::string category;  // source | station | transport | sink  (UI grouping)
    std::string description;
    std::vector<ParamSpec> params;
    std::function<std::unique_ptr<Node>(Plant&, const std::string& node_name, const json& params)> factory;
};

class NodeRegistry {
public:
    // Built-ins (source, machine, delay, batcher, assembler, sink) plus the custom
    // nodes from src/custom_nodes.cpp.
    [[nodiscard]] static NodeRegistry with_defaults();

    void add(NodeTypeInfo info);
    [[nodiscard]] const NodeTypeInfo* find(std::string_view name) const;
    [[nodiscard]] const std::vector<NodeTypeInfo>& types() const noexcept { return types_; }
    [[nodiscard]] json describe() const;  // schema for the UI / `factory_sim types`

private:
    std::vector<NodeTypeInfo> types_;
};

void register_builtin_nodes(NodeRegistry& registry);
void register_custom_nodes(NodeRegistry& registry);  // <- your nodes go here (see custom_nodes.cpp)

// Parameter schema fragments reusable by custom nodes.
[[nodiscard]] std::vector<ParamSpec> machine_param_specs();

}  // namespace factory
