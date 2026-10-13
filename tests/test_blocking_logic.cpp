#include "test_util.hpp"

using namespace testutil;

namespace {
// source -> fast (1 s, cap 1) -> slow (10 s, cap 1) -> sink
const char* kBackpressure = R"({
  "simulation": {"duration_seconds": 400, "random_seed": 1, "sample_interval": 0},
  "nodes": [
    {"id":"src","type":"source","interarrival":1,"max_parts":20,"on_blocked":"wait","downstream":["fast"]},
    {"id":"fast","type":"machine","capacity":1,"process":1,"downstream":["slow"]},
    {"id":"slow","type":"machine","capacity":1,"process":10,"downstream":["out"]},
    {"id":"out","type":"sink"}
  ]})";
}  // namespace

TEST_CASE("an upstream machine blocks when the downstream buffer is full, and resumes") {
    const Summary s = run(J(kBackpressure));
    CHECK(s.total.completed == 20);  // everything eventually drains: nothing deadlocks
    CHECK_FALSE(s.deadlock.has_value());
    CHECK(node(s, "fast").pct[2] > 30.0);  // spent a lot of time BLOCKED
    CHECK(node(s, "src").pct[2] > 30.0);   // and so did the source (wait mode)
}

TEST_CASE("buffers never exceed capacity (+1 in service)") {
    const Summary s = run(J(kBackpressure));
    CHECK(node(s, "fast").max_occupancy <= 2);
    CHECK(node(s, "slow").max_occupancy <= 2);
}

TEST_CASE("drop policy loses arrivals instead of blocking") {
    const json cfg = J(R"({
      "simulation": {"duration_seconds": 500, "random_seed": 1, "sample_interval": 0},
      "nodes": [
        {"id":"src","type":"source","interarrival":1,"on_blocked":"drop","downstream":["m"]},
        {"id":"m","type":"machine","capacity":2,"process":10,"downstream":["out"]},
        {"id":"out","type":"sink"}]})");
    const Summary s = run(cfg);
    CHECK(s.total.lost > 400);
    CHECK(node(s, "src").pct[2] == doctest::Approx(0.0));  // a dropping source never blocks
    CHECK(s.conservation_ok);
}

TEST_CASE("parallel stations share load under shortest-queue routing") {
    const json cfg = J(R"({
      "simulation": {"duration_seconds": 20000, "random_seed": 5, "sample_interval": 0},
      "nodes": [
        {"id":"src","type":"source","interarrival":{"dist":"exponential","mean":5},"downstream":["a","b"],
         "routing":{"policy":"shortest_queue"}},
        {"id":"a","type":"machine","capacity":10,"process":{"dist":"exponential","mean":8},"downstream":["out"]},
        {"id":"b","type":"machine","capacity":10,"process":{"dist":"exponential","mean":8},"downstream":["out"]},
        {"id":"out","type":"sink"}]})");
    const Summary s = run(cfg);
    const double a = static_cast<double>(node(s, "a").processed_units);
    const double b = static_cast<double>(node(s, "b").processed_units);
    CHECK(a / (a + b) == doctest::Approx(0.5).epsilon(0.08));
}

TEST_CASE("round robin spreads parts evenly") {
    const json cfg = J(R"({
      "simulation": {"duration_seconds": 1000, "random_seed": 5, "sample_interval": 0},
      "nodes": [
        {"id":"src","type":"source","interarrival":10,"max_parts":40,"downstream":["a","b","c"],
         "routing":{"policy":"round_robin"}},
        {"id":"a","type":"machine","capacity":-1,"process":1,"downstream":["out"]},
        {"id":"b","type":"machine","capacity":-1,"process":1,"downstream":["out"]},
        {"id":"c","type":"machine","capacity":-1,"process":1,"downstream":["out"]},
        {"id":"out","type":"sink"}]})");
    const Summary s = run(cfg);
    const auto a = node(s, "a").processed_units, b = node(s, "b").processed_units, c = node(s, "c").processed_units;
    CHECK(a + b + c == 40);
    CHECK(a >= 13);
    CHECK(b >= 13);
    CHECK(c >= 13);
}

TEST_CASE("a circular wait is detected as a deadlock and the cycle is reported") {
    BuildOptions o;
    o.duration_s = 100000;
    const Summary s = run(load_example("deadlock_demo.json"), o);
    REQUIRE(s.deadlock.has_value());
    CHECK(s.deadlock->cycle.find("press") != std::string::npos);
    CHECK(s.deadlock->cycle.find("check") != std::string::npos);
    CHECK_FALSE(s.warnings.empty());  // the loader warned about it up front
    CHECK(s.diagnosis.find("DEADLOCK") != std::string::npos);
}

TEST_CASE("a starved kit is not a deadlock: late arrivals of the missing part restart the line") {
    // Wheels arrive immediately and fill the assembler's wheel slots; the first frame only shows
    // up at t=100. The wheel conveyor blocks meanwhile, but a pending arrival can unblock it.
    const json cfg = J(R"({
      "simulation": {"duration_seconds": 2000, "random_seed": 1, "sample_interval": 0},
      "nodes": [
        {"id":"wheels","type":"source","interarrival":1,"mix":{"wheel":1},"on_blocked":"wait","downstream":["belt"]},
        {"id":"frames","type":"source","interarrival":100,"mix":{"frame":1},"max_parts":5,"on_blocked":"wait","downstream":["asm"]},
        {"id":"belt","type":"delay","transit":2,"downstream":["asm"]},
        {"id":"asm","type":"assembler","kit":{"frame":1,"wheel":2},"capacity":4,"process":5,"downstream":["out"]},
        {"id":"out","type":"sink"}]})");
    const Summary s = run(cfg);
    CHECK_FALSE(s.deadlock.has_value());
    CHECK(s.total.completed == 15);  // 5 bikes x (1 frame + 2 wheels)
    CHECK(s.conservation_ok);
}

TEST_CASE("a bypass rework lane removes the deadlock") {
    json cfg = load_example("deadlock_demo.json");
    for (auto& n : cfg["nodes"])
        if (n["id"] == "check") n["downstream"][1]["bypass"] = true;
    BuildOptions o;
    o.duration_s = 100000;
    const Summary s = run(cfg, o);
    CHECK_FALSE(s.deadlock.has_value());
    CHECK(s.warnings.empty());
    CHECK(s.total.completed > 100);
}
