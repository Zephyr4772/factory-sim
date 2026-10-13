#pragma once
#include <doctest/doctest.h>

#include <fstream>
#include <sstream>

#include "factory/analytics.hpp"

namespace testutil {

using namespace factory;

inline json J(const char* text) { return json::parse(text); }

inline const NodeRegistry& registry() {
    static const NodeRegistry r = NodeRegistry::with_defaults();
    return r;
}

inline Summary run(const json& cfg, BuildOptions opt = {}) { return run_once(cfg, registry(), opt); }

inline const NodeReport& node(const Summary& s, const std::string& id) {
    for (const auto& n : s.nodes)
        if (n.id == id) return n;
    FAIL("no node " << id);
    return s.nodes.front();
}

inline json load_example(const char* name) {
    std::ifstream in(std::string(FACTORY_EXAMPLES_DIR) + "/" + name, std::ios::binary);
    REQUIRE_MESSAGE(in.good(), "cannot open example " << name);
    std::ostringstream ss;
    ss << in.rdbuf();
    return json::parse(ss.str());
}

}  // namespace testutil
