#include "factory/nodes.hpp"
#include "test_util.hpp"

using namespace testutil;

namespace {
std::string wrap(const std::string& nodes, double duration = 2000) {
    return R"({"simulation":{"duration_seconds":)" + std::to_string(duration) +
           R"(,"random_seed":2,"sample_interval":0},"nodes":[)" + nodes + "]}";
}
}  // namespace

TEST_CASE("batcher forms full batches and conserves units") {
    const json cfg = json::parse(wrap(R"(
      {"id":"src","type":"source","interarrival":1,"max_parts":12,"downstream":["b"]},
      {"id":"b","type":"batcher","batch_size":4,"capacity":8,"process":2,"downstream":["out"]},
      {"id":"out","type":"sink"})"));
    const Summary s = run(cfg);
    CHECK(s.total.completed == 12);  // 3 batches x 4 units
    CHECK(node(s, "b").processed_units == 12);
    CHECK(s.conservation_ok);
}

TEST_CASE("batcher timeout releases a partial batch") {
    const json cfg = json::parse(wrap(R"(
      {"id":"src","type":"source","interarrival":1,"max_parts":3,"downstream":["b"]},
      {"id":"b","type":"batcher","batch_size":10,"capacity":20,"timeout":5,"process":1,"downstream":["out"]},
      {"id":"out","type":"sink"})"));
    const Summary s = run(cfg);
    CHECK(s.total.completed == 3);  // would be 0 without the timeout
    CHECK(s.wip_end == 0);
}

TEST_CASE("assembler waits for a complete kit and merges units") {
    const json cfg = json::parse(wrap(R"(
      {"id":"fa","type":"source","interarrival":10,"mix":{"frame":1},"max_parts":10,"downstream":["asm"]},
      {"id":"wa","type":"source","interarrival":5,"mix":{"wheel":1},"max_parts":20,"downstream":["asm"]},
      {"id":"asm","type":"assembler","kit":{"frame":1,"wheel":2},"output_type":"bike","capacity":4,"process":3,
       "downstream":["out"]},
      {"id":"out","type":"sink"})"));
    const Summary s = run(cfg);
    CHECK(s.total.completed == 30);  // 10 bikes x (1 frame + 2 wheels)
    CHECK(s.conservation_ok);
    CHECK(s.wip_end == 0);
}

TEST_CASE("assembler without one of its inputs never completes and keeps the other type bounded") {
    const json cfg = json::parse(wrap(R"(
      {"id":"fa","type":"source","interarrival":10,"mix":{"frame":1},"downstream":["asm"]},
      {"id":"asm","type":"assembler","kit":{"frame":1,"wheel":2},"capacity":3,"process":3,"downstream":["out"]},
      {"id":"out","type":"sink"})"));
    const Summary s = run(cfg);
    CHECK(s.total.completed == 0);
    CHECK(node(s, "asm").max_occupancy <= 3);
}

TEST_CASE("delay node adds exactly the transit time") {
    const json cfg = json::parse(wrap(R"(
      {"id":"src","type":"source","interarrival":10,"max_parts":20,"downstream":["belt"]},
      {"id":"belt","type":"delay","transit":5,"downstream":["out"]},
      {"id":"out","type":"sink"})"));
    const Summary s = run(cfg);
    CHECK(s.total.completed == 20);
    CHECK(s.cycle_time_s == doctest::Approx(5.0).epsilon(1e-9));
}

TEST_CASE("deterministic line: cycle time is the sum of service times") {
    const json cfg = json::parse(wrap(R"(
      {"id":"src","type":"source","interarrival":10,"max_parts":30,"downstream":["a"]},
      {"id":"a","type":"machine","capacity":3,"process":4,"downstream":["b"]},
      {"id":"b","type":"machine","capacity":3,"process":3,"downstream":["out"]},
      {"id":"out","type":"sink"})"));
    const Summary s = run(cfg);
    CHECK(s.total.completed == 30);
    CHECK(s.cycle_time_s == doctest::Approx(7.0).epsilon(1e-9));
}

TEST_CASE("routing rules send part types to different branches") {
    const json cfg = json::parse(wrap(R"(
      {"id":"src","type":"source","interarrival":2,"mix":{"A":1,"B":1},"max_parts":200,"downstream":["sa","sb"],
       "routing":{"rules":[{"match":{"type":"A"},"to":"sa"},{"to":"sb"}]}},
      {"id":"sa","type":"sink"},
      {"id":"sb","type":"sink","counts_as":"scrapped"})", 4000));
    const Summary s = run(cfg);
    CHECK(s.total.completed + s.total.scrapped == 200);
    CHECK(s.total.completed == node(s, "sa").processed_units);
    CHECK(s.total.completed > 70);
    CHECK(s.total.scrapped > 70);
}

TEST_CASE("per-type processing times and changeover setup apply") {
    // two types alternate through one machine; B takes 10 s, A takes 2 s, switching costs 5 s
    const json cfg = json::parse(wrap(R"(
      {"id":"src","type":"source","interarrival":100,"mix":{"A":1,"B":1},"max_parts":50,"downstream":["m"]},
      {"id":"m","type":"machine","capacity":-1,"process":1,
       "process_by_type":{"A":2,"B":10},"setup":5,"downstream":["out"]},
      {"id":"out","type":"sink"})", 6000));
    const Summary s = run(cfg);
    // mean cycle time = mean service (6) + setup (5) whenever the type switched (~50% of parts)
    CHECK(s.cycle_time_s > 7.0);
    CHECK(s.cycle_time_s < 10.5);
    CHECK(s.total.completed == 50);
}

TEST_CASE("drop-on-full machine counts lost units") {
    const json cfg = json::parse(wrap(R"(
      {"id":"src","type":"source","interarrival":1,"downstream":["m"]},
      {"id":"m","type":"machine","capacity":2,"on_full":"drop","process":10,"downstream":["out"]},
      {"id":"out","type":"sink"})", 300));
    const Summary s = run(cfg);
    CHECK(node(s, "m").lost_units > 200);
    CHECK(s.total.lost == node(s, "m").lost_units);
    CHECK(s.conservation_ok);
}

TEST_CASE("custom quality_gate: threshold 0 passes everything, 1 with no rework scraps everything") {
    auto gate = [](double threshold, int max_reworks) {
        return json::parse(wrap(R"(
          {"id":"src","type":"source","interarrival":5,"max_parts":100,"downstream":["g"]},
          {"id":"g","type":"quality_gate","capacity":5,"process":1,"threshold":)" + std::to_string(threshold) +
                                R"(,"max_reworks":)" + std::to_string(max_reworks) +
                                R"(,"pass_to":"ok","scrap_to":"bad","downstream":["ok","bad"]},
          {"id":"ok","type":"sink"},
          {"id":"bad","type":"sink","counts_as":"scrapped"})", 3000));
    };
    CHECK(run(gate(0.0, 2)).total.completed == 100);
    const Summary all_bad = run(gate(1.0, 0));
    CHECK(all_bad.total.completed == 0);
    CHECK(all_bad.total.scrapped == 100);
}

TEST_CASE("custom quality_gate with rework: reworked parts come back and eventually ship or scrap") {
    const Summary s = run(load_example("bike_assembly.json"));
    CHECK(s.conservation_ok);
    CHECK(s.total.completed > 100);
    CHECK(node(s, "bench").processed_units > 0);  // the rework path was exercised
    CHECK_FALSE(s.deadlock.has_value());
}

TEST_CASE("user-defined node types (templates) inherit and override parameters") {
    const json cfg = json::parse(R"({
      "simulation":{"duration_seconds":1000,"random_seed":1,"sample_interval":0},
      "node_types":{"slow_cnc":{"base":"machine","params":{"capacity":7,"process":20}},
                    "slower_cnc":{"base":"slow_cnc","params":{"process":40}}},
      "nodes":[
        {"id":"src","type":"source","interarrival":50,"downstream":["m"]},
        {"id":"m","type":"slower_cnc","downstream":["out"]},
        {"id":"out","type":"sink"}]})");
    auto plant = build_plant(cfg, registry());
    const json d = plant->node(*plant->find("m")).describe();
    CHECK(d["capacity"] == 7);                          // inherited from slow_cnc
    CHECK(d["process_mean"].get<double>() == doctest::Approx(40.0));  // overridden
}

TEST_CASE("a custom node type can be registered from C++") {
    NodeRegistry reg = NodeRegistry::with_defaults();
    NodeTypeInfo info;
    info.name = "turbo";
    info.category = "station";
    info.description = "test node: a machine with fixed fast service";
    info.factory = [](Plant& p, const std::string& name, const json& params) -> std::unique_ptr<Node> {
        ParamReader r(params, "node '" + name + "' (turbo)");
        r.finish();
        MachineConfig c;
        c.process = Distribution::constant(0.5);
        c.capacity = 3;
        return std::make_unique<Machine>(p, name, c);
    };
    reg.add(info);
    const json cfg = json::parse(wrap(R"(
      {"id":"src","type":"source","interarrival":5,"max_parts":10,"downstream":["t"]},
      {"id":"t","type":"turbo","downstream":["out"]},
      {"id":"out","type":"sink"})"));
    const Summary s = run_once(cfg, reg);
    CHECK(s.total.completed == 10);
    CHECK(s.cycle_time_s == doctest::Approx(0.5));
}
