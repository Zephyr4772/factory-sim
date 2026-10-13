#include "test_util.hpp"

using namespace testutil;

namespace {
std::string error_of(const json& cfg) {
    try {
        (void)build_plant(cfg, registry());
    } catch (const ConfigError& e) {
        return e.what();
    }
    return "";
}

const char* kBase = R"({
  "simulation":{"duration_seconds":100,"random_seed":1},
  "nodes":[
    {"id":"src","type":"source","interarrival":5,"downstream":["m"]},
    {"id":"m","type":"machine","capacity":3,"process":2,"downstream":["out"]},
    {"id":"out","type":"sink"}]})";
}  // namespace

TEST_CASE("every shipped example loads") {
    for (const char* f : {"sample_factory.json", "bike_assembly.json", "mixed_products.json", "legacy_spec.json",
                          "deadlock_demo.json"})
        CHECK_NOTHROW((void)build_plant(load_example(f), registry()));
}

TEST_CASE("the original station-based format is converted") {
    const Summary s = run(load_example("legacy_spec.json"));
    CHECK(s.total.completed > 100);
    CHECK(s.conservation_ok);
    bool found = false;
    for (const auto& n : s.nodes) found |= n.id == "__shipped";  // implicit sink added
    CHECK(found);
}

TEST_CASE("typos in parameters are rejected with the node and parameter name") {
    json cfg = J(kBase);
    cfg["nodes"][1]["capcity"] = 3;
    const std::string e = error_of(cfg);
    CHECK(e.find("node 'm'") != std::string::npos);
    CHECK(e.find("capcity") != std::string::npos);
}

TEST_CASE("structural errors are caught before simulating") {
    {
        json c = J(kBase);
        c["nodes"][0]["downstream"] = json::array({"nowhere"});
        CHECK(error_of(c).find("unknown target") != std::string::npos);
    }
    {
        json c = J(kBase);
        c["nodes"][1]["downstream"] = json::array();
        CHECK(error_of(c).find("no downstream") != std::string::npos);
    }
    {
        json c = J(kBase);
        c["nodes"].push_back(J(R"({"id":"m","type":"sink"})"));
        CHECK(error_of(c).find("duplicate") != std::string::npos);
    }
    {
        json c = J(kBase);
        c["nodes"].push_back(J(R"({"id":"orphan","type":"machine","downstream":["out"]})"));
        CHECK(error_of(c).find("unreachable") != std::string::npos);
    }
    {
        json c = J(kBase);
        c["nodes"][2]["downstream"] = json::array({"m"});
        CHECK_FALSE(error_of(c).empty());  // sink with downstream
    }
    {
        json c = J(kBase);
        c["nodes"][1]["downstream"][0] = J(R"({"to":"out","weight":-1})");
        CHECK(error_of(c).find("weight") != std::string::npos);
    }
    {
        json c = J(kBase);
        c["nodes"][1]["type"] = "warp_drive";
        CHECK(error_of(c).find("unknown node type") != std::string::npos);
    }
    {
        json c = J(kBase);
        c["nodes"][1]["mtbf"] = 100;  // mttr missing
        CHECK(error_of(c).find("mttr") != std::string::npos);
    }
    CHECK(error_of(J(R"({"nodes":[]})")).find("non-empty") != std::string::npos);
    CHECK(error_of(J(R"({"bogus":1,"nodes":[{"id":"a","type":"sink"}]})")).find("unknown top-level") != std::string::npos);
}

TEST_CASE("circular node_types and clashes with built-ins are rejected") {
    json c = J(kBase);
    c["node_types"] = J(R"({"a":{"base":"b"},"b":{"base":"a"}})");
    c["nodes"][1]["type"] = "a";
    CHECK(error_of(c).find("circular") != std::string::npos);
    json d = J(kBase);
    d["node_types"] = J(R"({"machine":{"base":"sink"}})");
    CHECK(error_of(d).find("clashes") != std::string::npos);
}

TEST_CASE("assembler and batcher sizing mistakes are caught") {
    json c = J(kBase);
    c["nodes"][1] = J(R"({"id":"m","type":"batcher","batch_size":5,"capacity":3,"downstream":["out"]})");
    CHECK(error_of(c).find("batch_size") != std::string::npos);
    json d = J(kBase);
    d["nodes"][1] = J(R"({"id":"m","type":"assembler","kit":{"a":4},"capacity":2,"downstream":["out"]})");
    CHECK(error_of(d).find("kit") != std::string::npos);
}

TEST_CASE("the same seed reproduces a run byte-for-byte, a different seed does not") {
    const json cfg = load_example("sample_factory.json");
    BuildOptions a, b;
    a.seed = 123;
    b.seed = 124;
    const std::string r1 = to_json(run(cfg, a)).dump();
    const std::string r2 = to_json(run(cfg, a)).dump();
    const std::string r3 = to_json(run(cfg, b)).dump();
    CHECK(r1 == r2);
    CHECK(r1 != r3);
}

TEST_CASE("adding an unrelated node does not disturb another node's random draws") {
    // Streams are keyed by node *name*, so 'm' behaves identically with or without 'spare'.
    json a = J(R"({"simulation":{"duration_seconds":5000,"random_seed":8,"sample_interval":0},"nodes":[
      {"id":"src","type":"source","interarrival":{"dist":"exponential","mean":9},"downstream":["m"]},
      {"id":"m","type":"machine","capacity":-1,"process":{"dist":"exponential","mean":6},"downstream":["out"]},
      {"id":"out","type":"sink"}]})");
    json b = a;
    b["nodes"].push_back(J(R"({"id":"spare","type":"machine","downstream":["out"]})"));
    // 'spare' must be reachable to pass validation, so route a one-in-a-trillion trickle to it
    b["nodes"][0]["downstream"] = J(R"([{"to":"m","weight":1000000},{"to":"spare","weight":0.000001}])");
    const Summary sa = run(a), sb = run(b);
    CHECK(node(sa, "m").processed_units == node(sb, "m").processed_units);
}
