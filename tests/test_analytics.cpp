#include <filesystem>

#include "test_util.hpp"

using namespace testutil;

namespace {
// source -> machine -> sink with exponential arrivals/service: textbook M/M/1(/K).
json mm1(double mean_arrival, double mean_service, int capacity, const char* on_full, double duration,
         double warmup = 2000) {
    return json::parse(R"({"simulation":{"duration_seconds":)" + std::to_string(duration) + R"(,"warmup_seconds":)" +
                       std::to_string(warmup) + R"(,"random_seed":17,"sample_interval":0},"nodes":[
      {"id":"src","type":"source","interarrival":{"dist":"exponential","mean":)" + std::to_string(mean_arrival) +
                       R"(},"downstream":["m"]},
      {"id":"m","type":"machine","capacity":)" + std::to_string(capacity) + R"(,"on_full":")" + on_full +
                       R"(","process":{"dist":"exponential","mean":)" + std::to_string(mean_service) +
                       R"(},"downstream":["out"]},
      {"id":"out","type":"sink"}]})");
}
}  // namespace

TEST_CASE("M/M/1 matches queueing theory: L = rho/(1-rho), W = 1/(mu-lambda)") {
    // lambda = 0.1/s, mu = 1/7 per s -> rho = 0.7
    const Summary s = run(mm1(10, 7, -1, "block", 1'500'000));
    CHECK(s.avg_wip == doctest::Approx(0.7 / 0.3).epsilon(0.06));
    CHECK(s.cycle_time_s == doctest::Approx(1.0 / (1.0 / 7.0 - 0.1)).epsilon(0.06));
    CHECK(node(s, "m").pct[1] == doctest::Approx(70.0).epsilon(0.03));  // utilisation = rho
}

TEST_CASE("Little's Law holds on a long run") {
    const Summary s = run(mm1(10, 7, -1, "block", 1'500'000));
    CHECK(s.little.gap_pct < 1.0);
}

TEST_CASE("M/M/1/K blocking probability matches the closed form") {
    // rho = 0.8, system size K = capacity(3) + 1 in service = 4: P_K = (1-rho) rho^K / (1 - rho^(K+1))
    const double rho = 0.8, K = 4;
    const double expected = (1 - rho) * std::pow(rho, K) / (1 - std::pow(rho, K + 1));
    const Summary s = run(mm1(10, 8, 3, "drop", 1'500'000));
    CHECK(s.lost_pct / 100.0 == doctest::Approx(expected).epsilon(0.05));
}

TEST_CASE("a saturated tandem line is limited by its slowest station") {
    const json cfg = J(R"({"simulation":{"duration_seconds":40000,"warmup_seconds":2000,"random_seed":1,"sample_interval":0},
      "nodes":[
        {"id":"src","type":"source","interarrival":1,"on_blocked":"wait","downstream":["fast"]},
        {"id":"fast","type":"machine","capacity":4,"process":5,"downstream":["slow"]},
        {"id":"slow","type":"machine","capacity":4,"process":8,"downstream":["out"]},
        {"id":"out","type":"sink"}]})");
    const Summary s = run(cfg);
    CHECK(s.uph == doctest::Approx(3600.0 / 8.0).epsilon(0.002));
    CHECK(s.bottleneck == "slow");
}

TEST_CASE("conservation of mass holds on every example, across seeds") {
    for (const char* f : {"sample_factory.json", "bike_assembly.json", "mixed_products.json", "legacy_spec.json"}) {
        const json cfg = load_example(f);
        for (std::uint64_t seed = 1; seed <= 4; ++seed) {
            BuildOptions o;
            o.seed = seed;
            const Summary s = run(cfg, o);
            INFO(f << " seed " << seed);
            CHECK(s.conservation_ok);
            CHECK(s.total.created == s.total.completed + s.total.scrapped + s.total.lost + static_cast<std::uint64_t>(s.wip_end));
        }
    }
}

TEST_CASE("every shipped example runs cleanly: no deadlock, units conserved, a bottleneck is named") {
    namespace fs = std::filesystem;
    int checked = 0;
    for (const auto& entry : fs::directory_iterator(FACTORY_EXAMPLES_DIR)) {
        if (entry.path().extension() != ".json") continue;
        const std::string name = entry.path().filename().string();
        if (name == "deadlock_demo.json") continue;  // deadlocks on purpose
        INFO(name);
        const Summary s = run(load_example(name.c_str()));
        CHECK_FALSE(s.deadlock.has_value());
        CHECK(s.conservation_ok);
        CHECK(s.total.completed > 0);
        CHECK_FALSE(s.bottleneck.empty());
        CHECK(s.warnings.empty());
        ++checked;
    }
    CHECK(checked >= 15);
}

TEST_CASE("warm-up resets the statistics window but not the totals") {
    const json cfg = mm1(10, 7, -1, "block", 20000, 5000);
    auto plant = build_plant(cfg, registry());
    plant->run();
    CHECK(plant->window().created < plant->total().created);
    CHECK(to_seconds(plant->window_start()) == doctest::Approx(5000.0));
}

TEST_CASE("what-if analysis finds the bottleneck as the best lever and reports a measured gain") {
    const json cfg = J(R"({"simulation":{"duration_seconds":30000,"warmup_seconds":1000,"random_seed":1,"sample_interval":0},
      "nodes":[
        {"id":"src","type":"source","interarrival":1,"on_blocked":"wait","downstream":["fast"]},
        {"id":"fast","type":"machine","capacity":4,"process":5,"downstream":["slow"]},
        {"id":"slow","type":"machine","capacity":4,"process":8,"downstream":["out"]},
        {"id":"out","type":"sink"}]})");
    const WhatIfReport w = what_if(cfg, registry(), 2);
    REQUIRE_FALSE(w.results.empty());
    CHECK(w.results.front().node == "slow");
    CHECK(w.results.front().change == "10% faster processing");
    CHECK(w.results.front().delta_pct == doctest::Approx(11.1).epsilon(0.1));  // 8s -> 7.2s is +11.1% rate
}

TEST_CASE("replications give a confidence interval that brackets the true mean") {
    const ReplicationReport r = replicate(mm1(10, 7, -1, "block", 100000), registry(), 12);
    CHECK(r.n == 12);
    CHECK(r.uph.ci95 > 0.0);
    CHECK(r.uph.mean == doctest::Approx(360.0).epsilon(0.05));  // 1 part / 10 s
    CHECK(r.deadlocks == 0);
}

TEST_CASE("demand-limited lines are diagnosed as such") {
    const Summary s = run(mm1(100, 5, 10, "block", 50000));
    CHECK(s.demand_limited);
    CHECK(s.diagnosis.find("keeps up with demand") != std::string::npos);
}

TEST_CASE("trace export lists state/queue/move events and node names") {
    BuildOptions o;
    o.trace = true;
    o.duration_s = 300;
    json cfg = load_example("sample_factory.json");
    cfg["simulation"].erase("warmup_seconds");  // the 300 s override is shorter than the example's warm-up
    auto plant = build_plant(cfg, registry(), o);
    plant->run();
    const json t = trace_to_json(*plant);
    CHECK(t["events"].size() > 50);
    CHECK(t["nodes"].size() == plant->node_count());
    CHECK(t["part_types"].size() >= 1);
}

TEST_CASE("ASCII report and CSV export are well-formed") {
    auto plant = build_plant(load_example("sample_factory.json"), registry());
    plant->run();
    const std::string table = to_ascii(summarize(*plant));
    CHECK(table.find("Little's Law") != std::string::npos);
    CHECK(table.find("milling") != std::string::npos);
    const std::string csv = samples_csv(*plant);
    CHECK(csv.rfind("time_s,wip_units", 0) == 0);
    CHECK(std::count(csv.begin(), csv.end(), '\n') > 100);
}
