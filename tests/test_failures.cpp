#include "test_util.hpp"

using namespace testutil;

namespace {
json one_machine(const std::string& extra, double duration = 200000) {
    const std::string text = R"({"simulation": {"duration_seconds": )" + std::to_string(duration) +
                             R"(, "random_seed": 9, "sample_interval": 0},
      "nodes": [
        {"id":"src","type":"source","interarrival":1,"on_blocked":"wait","downstream":["m"]},
        {"id":"m","type":"machine","capacity":2,"process":10,)" + extra + R"("downstream":["out"]},
        {"id":"out","type":"sink"}]})";
    return json::parse(text);
}
}  // namespace

TEST_CASE("downtime matches MTTR/(MTBF+MTTR) for a saturated machine") {
    const Summary s = run(one_machine(R"("mtbf":100,"mttr":50,)"));
    const NodeReport& m = node(s, "m");
    CHECK(m.pct[3] == doctest::Approx(100.0 * 50.0 / 150.0).epsilon(0.06));
    CHECK(m.failures > 1000);
}

TEST_CASE("a failed machine produces nothing: output is bounded by busy time") {
    const Summary s = run(one_machine(R"("mtbf":100,"mttr":50,)"));
    const double busy_s = node(s, "m").pct[1] / 100.0 * s.window_s;
    CHECK(static_cast<double>(s.total.completed) * 10.0 <= busy_s + 10.0);
    const Summary base = run(one_machine(""));  // unbroken machine for comparison
    CHECK(s.total.completed < base.total.completed * 8 / 10);
}

TEST_CASE("state times add up to the whole window") {
    const Summary s = run(one_machine(R"("mtbf":100,"mttr":50,)", 5000));
    const NodeReport& m = node(s, "m");
    CHECK(m.pct[0] + m.pct[1] + m.pct[2] + m.pct[3] == doctest::Approx(100.0).epsilon(1e-9));
}

TEST_CASE("resume vs restart: restarting wastes work") {
    const Summary resume = run(one_machine(R"("mtbf":60,"mttr":30,"on_failure":"resume",)"));
    const Summary restart = run(one_machine(R"("mtbf":60,"mttr":30,"on_failure":"restart",)"));
    CHECK(restart.total.completed < resume.total.completed);
}

TEST_CASE("scrap policy destroys the part in progress but conserves units") {
    const Summary s = run(one_machine(R"("mtbf":80,"mttr":20,"on_failure":"scrap",)", 50000));
    CHECK(s.total.scrapped > 100);
    CHECK(s.conservation_ok);
}

TEST_CASE("operating clock: an idle machine does not wear out; wall clock: it does") {
    // The machine is busy only 10% of the time (arrival every 100 s, service 10 s).
    auto cfg = [](const std::string& clock) {
        const std::string t = R"({"simulation":{"duration_seconds":400000,"random_seed":4,"sample_interval":0},
          "nodes":[
            {"id":"src","type":"source","interarrival":100,"downstream":["m"]},
            {"id":"m","type":"machine","capacity":5,"process":10,"mtbf":200,"mttr":50,"failure_clock":")" +
                              clock + R"(","downstream":["out"]},
            {"id":"out","type":"sink"}]})";
        return json::parse(t);
    };
    const Summary op = run(cfg("operating"));
    const Summary wall = run(cfg("wall"));
    CHECK(node(wall, "m").failures > 3 * node(op, "m").failures);
}

TEST_CASE("exactly-once accounting survives heavy failure churn (property over seeds)") {
    for (std::uint64_t seed = 1; seed <= 12; ++seed) {
        BuildOptions o;
        o.seed = seed;
        const Summary s = run(one_machine(R"("mtbf":15,"mttr":15,"on_failure":"restart",)", 3000), o);
        CHECK(s.conservation_ok);
    }
}
