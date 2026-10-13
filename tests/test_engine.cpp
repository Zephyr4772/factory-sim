#include "test_util.hpp"

using namespace factory;

TEST_CASE("events fire in chronological order, ties in scheduling order") {
    EventEngine eng;
    eng.schedule(300, EventType::Timer, 1);
    eng.schedule(100, EventType::PartSpawned, 2);
    eng.schedule(200, EventType::Sample, 3);
    eng.schedule(100, EventType::MachineFailed, 4);  // same time as node 2, scheduled later
    eng.schedule(100, EventType::MachineRepaired, 5);

    Event e;
    std::vector<NodeId> order;
    Tick last = 0;
    while (eng.pop(e)) {
        CHECK(e.time >= last);
        last = e.time;
        order.push_back(e.node);
    }
    CHECK(order == std::vector<NodeId>{2, 4, 5, 3, 1});
}

TEST_CASE("the clock jumps to the event time and never runs backwards") {
    EventEngine eng;
    eng.schedule(5'000'000, EventType::Timer);
    Event e;
    REQUIRE(eng.pop(e));
    CHECK(eng.now() == 5'000'000);
    eng.schedule(1, EventType::Timer);  // in the past: clamped to now
    REQUIRE(eng.pop(e));
    CHECK(e.time == 5'000'000);
    CHECK(eng.now() == 5'000'000);
}

TEST_CASE("pending counters track each event type") {
    EventEngine eng;
    eng.schedule(10, EventType::Timer);
    eng.schedule(20, EventType::Timer);
    eng.schedule(30, EventType::Sample);
    CHECK(eng.pending(EventType::Timer) == 2);
    CHECK(eng.pending() == 3);
    Event e;
    REQUIRE(eng.pop(e));
    CHECK(eng.pending(EventType::Timer) == 1);
    CHECK(eng.processed() == 1);
}

TEST_CASE("time conversion round-trips") {
    CHECK(to_ticks(1.5) == 1'500'000);
    CHECK(to_seconds(2'500'000) == doctest::Approx(2.5));
}

TEST_CASE("per-node RNG streams are independent of each other and reproducible") {
    RngStream a1(42, "mill/process"), a2(42, "mill/process"), b(42, "lathe/process");
    CHECK(a1.uniform() == a2.uniform());
    CHECK(a1.uniform() != b.uniform());
    for (int i = 0; i < 1000; ++i) {
        const double u = a1.uniform();
        CHECK(u >= 0.0);
        CHECK(u < 1.0);
    }
}

TEST_CASE("distributions: sample means converge and truncated normal stays positive") {
    RngStream rng(1, "dist");
    auto mean_of = [&](const Distribution& d) {
        double s = 0;
        for (int i = 0; i < 200000; ++i) s += d.sample(rng);
        return s / 200000;
    };
    CHECK(mean_of(Distribution::exponential(8.0)) == doctest::Approx(8.0).epsilon(0.02));
    CHECK(mean_of(Distribution::normal(10.0, 2.0)) == doctest::Approx(10.0).epsilon(0.01));
    CHECK(mean_of({Distribution::Kind::Uniform, 2, 6, 0}) == doctest::Approx(4.0).epsilon(0.01));
    CHECK(mean_of({Distribution::Kind::Triangular, 1, 2, 6}) == doctest::Approx(3.0).epsilon(0.02));
    CHECK(mean_of({Distribution::Kind::LogNormal, 5, 2, 0}) == doctest::Approx(5.0).epsilon(0.02));
    const Distribution wide = Distribution::normal(1.0, 5.0);  // would go negative often
    for (int i = 0; i < 10000; ++i) CHECK(wide.sample(rng) > 0.0);
}
