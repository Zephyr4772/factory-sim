#include "factory/analytics.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <iomanip>
#include <sstream>
#include <thread>

namespace factory {

namespace {

constexpr double kAlpha = 0.1;  // weight of "downstream stations are starved" (tie-breaker)
constexpr double kBeta = 0.1;   // weight of "upstream stations are blocked" (tie-breaker)

void parallel_for(std::size_t n, const std::function<void(std::size_t)>& fn) {
    const std::size_t workers = std::max<std::size_t>(1, std::min<std::size_t>(n, std::thread::hardware_concurrency()));
    std::atomic<std::size_t> next{0};
    std::vector<std::thread> pool;
    for (std::size_t w = 0; w < workers; ++w)
        pool.emplace_back([&] {
            for (std::size_t i = next++; i < n; i = next++) fn(i);
        });
    for (auto& t : pool) t.join();
}

std::string fixed(double v, int prec) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

}  // namespace

// ------------------------------------------------------------------ summary

Summary summarize(const Plant& plant) {
    Summary s;
    const Tick window_ticks = plant.end_time() - plant.window_start();
    s.window_s = to_seconds(window_ticks);
    s.window = plant.window();
    s.total = plant.total();
    s.wip_end = plant.wip_units();
    const double hours = s.window_s / 3600.0;
    s.uph = hours > 0 ? static_cast<double>(s.window.completed) / hours : 0;
    s.avg_wip = window_ticks > 0 ? s.window.wip_area / static_cast<double>(window_ticks) : 0;
    s.cycle_time_s = s.window.departed > 0 ? to_seconds(s.window.sojourn) / static_cast<double>(s.window.departed) : 0;
    s.lost_pct = s.window.created > 0 ? 100.0 * static_cast<double>(s.window.lost) / static_cast<double>(s.window.created) : 0;
    s.conservation_ok = s.total.created == s.total.completed + s.total.scrapped + s.total.lost +
                                               static_cast<std::uint64_t>(s.wip_end);

    s.little.L = s.avg_wip;
    s.little.lambda = s.window_s > 0 ? static_cast<double>(s.window.departed) / s.window_s : 0;
    s.little.W = s.cycle_time_s;
    s.little.gap_pct = s.little.L > 0 ? 100.0 * std::abs(s.little.L - s.little.lambda * s.little.W) / s.little.L : 0;

    // ---- per node ----
    const std::size_t n = plant.node_count();
    s.nodes.resize(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Node& node = plant.node(static_cast<NodeId>(i));
        const NodeStats st = node.stats_snapshot();
        NodeReport& r = s.nodes[i];
        r.id = node.name();
        r.kind = std::string(node.kind());
        r.station = node.is_station();
        for (std::size_t k = 0; k < kStateCount; ++k)
            r.pct[k] = window_ticks > 0 ? 100.0 * static_cast<double>(st.time_in_state[k]) / static_cast<double>(window_ticks) : 0;
        r.avg_occupancy = window_ticks > 0 ? st.occupancy_area / static_cast<double>(window_ticks) : 0;
        r.max_occupancy = st.max_occupancy;
        r.capacity = node.capacity();
        r.processed_units = st.processed_units;
        r.lost_units = st.lost_units;
        r.failures = st.failures;
        r.avg_wait_s = st.wait_count ? to_seconds(st.wait_sum) / static_cast<double>(st.wait_count) : 0;
        r.mtbf_s = node.describe().value("mtbf", 0.0);
        if (r.kind == "source") s.sources_blocked_pct = std::max(s.sources_blocked_pct, r.pct[2]);
    }
    s.demand_limited = s.lost_pct < 0.5 && s.sources_blocked_pct < 5.0;

    // ---- bottleneck index: Busy% + a*avg(downstream starved%) + b*avg(upstream blocked%) ----
    constexpr auto IDLE = static_cast<std::size_t>(NodeState::Idle);
    constexpr auto BUSY = static_cast<std::size_t>(NodeState::Busy);
    constexpr auto DOWN = static_cast<std::size_t>(NodeState::Down);
    constexpr auto BLOCKED = static_cast<std::size_t>(NodeState::Blocked);
    std::vector<std::vector<std::size_t>> up(n), down(n);
    for (std::size_t i = 0; i < n; ++i)
        for (const Edge& e : plant.node(static_cast<NodeId>(i)).edges()) {
            const auto t = static_cast<std::size_t>(e.to);
            if (!s.nodes[i].station || !s.nodes[t].station || t == i) continue;
            if (std::find(down[i].begin(), down[i].end(), t) == down[i].end()) down[i].push_back(t);
            if (std::find(up[t].begin(), up[t].end(), i) == up[t].end()) up[t].push_back(i);
        }
    auto avg = [&](const std::vector<std::size_t>& ids, std::size_t state) {
        if (ids.empty()) return 0.0;
        double sum = 0;
        for (std::size_t id : ids) sum += s.nodes[id].pct[state];
        return sum / static_cast<double>(ids.size());
    };
    // A machine under repair still consumes capacity, so utilisation counts BUSY and DOWN.
    double best_index = -1;
    for (std::size_t i = 0; i < n; ++i) {
        if (!s.nodes[i].station) continue;
        s.nodes[i].bottleneck_index =
            s.nodes[i].pct[BUSY] + s.nodes[i].pct[DOWN] + kAlpha * avg(down[i], IDLE) + kBeta * avg(up[i], BLOCKED);
        if (s.nodes[i].bottleneck_index > best_index) {
            best_index = s.nodes[i].bottleneck_index;
            s.bottleneck = s.nodes[i].id;
        }
    }

    s.deadlock = plant.deadlock();
    s.warnings = plant.warnings();
    s.samples = plant.samples();

    // ---- diagnosis ----
    std::ostringstream d;
    if (s.deadlock) {
        d << "DEADLOCK at t=" << fixed(to_seconds(s.deadlock->at), 1) << "s: every remaining node is waiting on another.";
        if (!s.deadlock->cycle.empty()) d << " Wait-for cycle: " << s.deadlock->cycle << ".";
        d << " Break the loop with a rework bypass lane (\"bypass\": true) or an unbounded buffer.";
    } else if (!s.bottleneck.empty()) {
        const NodeReport* b = nullptr;
        for (const auto& r : s.nodes)
            if (r.id == s.bottleneck) b = &r;
        if (s.demand_limited) {
            d << "The line keeps up with demand (" << fixed(s.lost_pct, 2) << "% lost, sources never throttled): throughput equals the arrival rate, so nothing is binding right now. "
              << "The busiest station is '" << b->id << "' at " << fixed(b->pct[BUSY], 0) << "% busy; that is where capacity runs out first as demand grows.";
        } else {
            d << "Primary bottleneck: '" << b->id << "' (" << fixed(b->pct[BUSY], 0) << "% busy, " << fixed(b->pct[static_cast<std::size_t>(NodeState::Down)], 0)
              << "% down, index " << fixed(b->bottleneck_index, 0) << "). ";
            if (s.lost_pct >= 0.5) d << fixed(s.lost_pct, 1) << "% of arrivals are lost. ";
            if (s.sources_blocked_pct >= 5.0) d << "Sources are held back up to " << fixed(s.sources_blocked_pct, 0) << "% of the time. ";
            d << "Run what-if to measure which lever recovers the most throughput.";
        }
    }
    s.diagnosis = d.str();
    return s;
}

json to_json(const Summary& s) {
    json nodes = json::array();
    for (const auto& r : s.nodes)
        nodes.push_back({{"id", r.id},
                         {"kind", r.kind},
                         {"station", r.station},
                         {"idle_pct", r.pct[0]},
                         {"busy_pct", r.pct[1]},
                         {"blocked_pct", r.pct[2]},
                         {"down_pct", r.pct[3]},
                         {"avg_occupancy", r.avg_occupancy},
                         {"max_occupancy", r.max_occupancy},
                         {"capacity", r.capacity},
                         {"processed_units", r.processed_units},
                         {"lost_units", r.lost_units},
                         {"failures", r.failures},
                         {"avg_wait_s", r.avg_wait_s},
                         {"bottleneck_index", r.bottleneck_index}});
    json samples = json::array();
    for (const auto& sm : s.samples) samples.push_back({to_seconds(sm.t), sm.wip, sm.completed});
    json out = {{"window_s", s.window_s},
                {"created", s.total.created},
                {"completed", s.total.completed},
                {"scrapped", s.total.scrapped},
                {"lost", s.total.lost},
                {"wip_end", s.wip_end},
                {"uph", s.uph},
                {"avg_wip", s.avg_wip},
                {"cycle_time_s", s.cycle_time_s},
                {"lost_pct", s.lost_pct},
                {"conservation_ok", s.conservation_ok},
                {"demand_limited", s.demand_limited},
                {"little", {{"L", s.little.L}, {"lambda", s.little.lambda}, {"W", s.little.W}, {"gap_pct", s.little.gap_pct}}},
                {"nodes", nodes},
                {"bottleneck", s.bottleneck},
                {"sources_blocked_pct", s.sources_blocked_pct},
                {"diagnosis", s.diagnosis},
                {"warnings", s.warnings},
                {"samples", samples},
                {"deadlock", nullptr}};
    if (s.deadlock)
        out["deadlock"] = {{"at_s", to_seconds(s.deadlock->at)}, {"blocked", s.deadlock->blocked}, {"cycle", s.deadlock->cycle}};
    return out;
}

std::string to_ascii(const Summary& s) {
    std::ostringstream o;
    const std::string rule(86, '=');
    o << rule << "\n";
    o << " FACTORY SIMULATION RESULTS   window " << fixed(s.window_s / 3600.0, 2) << " h\n" << rule << "\n";
    o << " Units   created " << s.total.created << "   completed " << s.total.completed << "   scrapped " << s.total.scrapped
      << "   lost " << s.total.lost << "   in-system " << s.wip_end << "   conservation "
      << (s.conservation_ok ? "OK" : "VIOLATED") << "\n";
    o << " Throughput " << fixed(s.uph, 1) << " units/hour   avg WIP " << fixed(s.avg_wip, 2) << "   cycle time "
      << fixed(s.cycle_time_s, 1) << " s\n";
    o << " Little's Law  L=" << fixed(s.little.L, 3) << "  lambda*W=" << fixed(s.little.lambda * s.little.W, 3) << "  gap "
      << fixed(s.little.gap_pct, 2) << "% (units still in flight at the cut-off)\n";
    o << std::string(86, '-') << "\n";
    o << std::left << std::setw(16) << " node" << std::setw(13) << "kind" << std::right << std::setw(8) << "busy%"
      << std::setw(9) << "starved%" << std::setw(9) << "blocked%" << std::setw(7) << "down%" << std::setw(8) << "avgQ"
      << std::setw(6) << "maxQ" << std::setw(8) << "index" << "\n";
    for (const auto& r : s.nodes) {
        const bool mark = r.id == s.bottleneck && !s.deadlock;
        o << std::left << std::setw(16) << ((mark ? "*" : " ") + r.id).substr(0, 15) << std::setw(13) << r.kind << std::right
          << std::setw(8) << fixed(r.pct[1], 1) << std::setw(9) << fixed(r.pct[0], 1) << std::setw(9) << fixed(r.pct[2], 1)
          << std::setw(7) << fixed(r.pct[3], 1) << std::setw(8) << fixed(r.avg_occupancy, 2) << std::setw(6) << r.max_occupancy;
        if (r.station) o << std::setw(8) << fixed(r.bottleneck_index, 0);
        o << "\n";
    }
    o << std::string(86, '-') << "\n";
    if (!s.diagnosis.empty()) o << " " << s.diagnosis << "\n";
    for (const auto& w : s.warnings) o << " warning: " << w << "\n";
    o << rule << "\n";
    return o.str();
}

std::string samples_csv(const Plant& plant) {
    std::ostringstream o;
    o << "time_s,wip_units,completed_units,throughput_uph_cumulative\n";
    for (const auto& sm : plant.samples()) {
        const double t = to_seconds(sm.t);
        o << fixed(t, 1) << "," << sm.wip << "," << sm.completed << "," << fixed(t > 0 ? static_cast<double>(sm.completed) / (t / 3600.0) : 0.0, 2) << "\n";
    }
    return o.str();
}

json trace_to_json(const Plant& plant) {
    json events = json::array();
    for (const TraceEvent& e : plant.trace())
        events.push_back({e.t / 1000, static_cast<int>(e.kind), e.a, e.b, e.c});  // [t_ms, kind, a, b, c]
    json nodes = json::array();
    for (std::size_t i = 0; i < plant.node_count(); ++i) nodes.push_back(plant.node(static_cast<NodeId>(i)).name());
    return {{"events", events},
            {"nodes", nodes},
            {"part_types", plant.type_names()},
            {"truncated", plant.trace_truncated()},
            {"t_end_ms", plant.end_time() / 1000}};
}

// ------------------------------------------------------------------ batch runs

Summary run_once(const json& config, const NodeRegistry& registry, const BuildOptions& options) {
    auto plant = build_plant(config, registry, options);
    plant->run();
    return summarize(*plant);
}

namespace {
Stat make_stat(const std::vector<double>& v) {
    Stat s;
    if (v.empty()) return s;
    for (double x : v) s.mean += x;
    s.mean /= static_cast<double>(v.size());
    if (v.size() > 1) {
        double ss = 0;
        for (double x : v) ss += (x - s.mean) * (x - s.mean);
        s.sd = std::sqrt(ss / static_cast<double>(v.size() - 1));
        static constexpr double t95[] = {12.706, 4.303, 3.182, 2.776, 2.571, 2.447, 2.365, 2.306, 2.262, 2.228,
                                         2.201,  2.179, 2.160, 2.145, 2.131, 2.120, 2.110, 2.101, 2.093, 2.086,
                                         2.080,  2.074, 2.069, 2.064, 2.060, 2.056, 2.052, 2.048, 2.045, 2.042};
        const std::size_t df = v.size() - 1;
        const double t = df <= 30 ? t95[df - 1] : 1.96;
        s.ci95 = t * s.sd / std::sqrt(static_cast<double>(v.size()));
    }
    return s;
}

std::uint64_t base_seed(const json& cfg) {
    const json norm = normalize_config(cfg);
    if (norm.contains("simulation") && norm["simulation"].contains("random_seed"))
        return static_cast<std::uint64_t>(norm["simulation"]["random_seed"].get<double>());
    return 42;
}
}  // namespace

ReplicationReport replicate(const json& config, const NodeRegistry& registry, int reps) {
    reps = std::max(1, reps);
    const std::uint64_t seed0 = base_seed(config);
    std::vector<Summary> results(static_cast<std::size_t>(reps));
    std::vector<std::string> errors(static_cast<std::size_t>(reps));
    parallel_for(static_cast<std::size_t>(reps), [&](std::size_t i) {
        try {
            BuildOptions o;
            o.seed = seed0 + i;
            results[i] = run_once(config, registry, o);
        } catch (const std::exception& e) {
            errors[i] = e.what();
        }
    });
    for (const auto& e : errors)
        if (!e.empty()) throw ConfigError(e);
    ReplicationReport rep;
    rep.n = reps;
    std::vector<double> uph, wip, ct, lost;
    for (const auto& r : results) {
        uph.push_back(r.uph);
        wip.push_back(r.avg_wip);
        ct.push_back(r.cycle_time_s);
        lost.push_back(r.lost_pct);
        rep.deadlocks += r.deadlock ? 1 : 0;
    }
    rep.uph = make_stat(uph);
    rep.wip = make_stat(wip);
    rep.cycle_time_s = make_stat(ct);
    rep.lost_pct = make_stat(lost);
    return rep;
}

json to_json(const ReplicationReport& r) {
    auto st = [](const Stat& s) { return json{{"mean", s.mean}, {"sd", s.sd}, {"ci95", s.ci95}}; };
    return {{"n", r.n}, {"uph", st(r.uph)}, {"avg_wip", st(r.wip)}, {"cycle_time_s", st(r.cycle_time_s)},
            {"lost_pct", st(r.lost_pct)}, {"deadlocks", r.deadlocks}};
}

// ------------------------------------------------------------------ what-if

namespace {
void set_param(json& node, const std::string& key, const json& value) {
    if (node.contains(key)) node[key] = value;  // loose key wins over params when both exist
    else node["params"][key] = value;
}
json* find_node(json& cfg, const std::string& id) {
    for (json& n : cfg["nodes"])
        if (n.value("id", "") == id) return &n;
    return nullptr;
}
}  // namespace

WhatIfReport what_if(const json& config, const NodeRegistry& registry, int reps) {
    reps = std::max(1, reps);
    json base = normalize_config(config);
    if (!base.contains("simulation")) base["simulation"] = json::object();
    const std::uint64_t seed0 = base_seed(config);

    // Differences between levers are small next to run-to-run noise, so each measurement
    // uses at least 8 simulated hours (the user's duration if longer).
    double base_duration = 3600;
    if (base.contains("simulation")) base_duration = base["simulation"].value("duration_seconds", base_duration);
    const double duration = std::max(base_duration, 28800.0);
    auto avg_uph = [&](const json& cfg) {
        std::vector<double> v(static_cast<std::size_t>(reps));
        parallel_for(static_cast<std::size_t>(reps), [&](std::size_t i) {
            json c = cfg;
            if (!c["simulation"].contains("warmup_seconds")) c["simulation"]["warmup_seconds"] = 1800;
            BuildOptions o;
            o.seed = seed0 + i;
            o.duration_s = duration;
            v[i] = run_once(c, registry, o).uph;
        });
        double sum = 0;
        for (double x : v) sum += x;
        return sum / static_cast<double>(reps);
    };

    BuildOptions first;
    first.seed = seed0;
    const Summary baseline = run_once(base, registry, first);

    // Candidates: the three highest-index stations.
    std::vector<const NodeReport*> stations;
    for (const auto& r : baseline.nodes)
        if (r.station) stations.push_back(&r);
    std::sort(stations.begin(), stations.end(), [](auto* a, auto* b) { return a->bottleneck_index > b->bottleneck_index; });
    if (stations.size() > 3) stations.resize(3);

    struct Experiment {
        std::string node, change;
        json cfg;
    };
    std::vector<Experiment> experiments;
    for (const NodeReport* st : stations) {
        auto make = [&](const std::string& change, const std::function<void(json&)>& patch) {
            json c = base;
            if (json* n = find_node(c, st->id)) patch(*n);
            experiments.push_back({st->id, change, std::move(c)});
        };
        if (st->capacity >= 0)
            make("+2 buffer slots", [&](json& n) { set_param(n, "capacity", st->capacity + 2); });
        make("10% faster processing", [](json& n) { set_param(n, "process_scale", 0.9); });
        if (st->mtbf_s > 0) make("20% shorter repairs", [](json& n) { set_param(n, "repair_scale", 0.8); });
    }

    WhatIfReport rep;
    rep.reps = reps;
    rep.base_uph = avg_uph(base);
    std::vector<double> uph(experiments.size());
    // Each experiment already parallelises over seeds; run experiments one after another.
    for (std::size_t i = 0; i < experiments.size(); ++i) uph[i] = avg_uph(experiments[i].cfg);
    for (std::size_t i = 0; i < experiments.size(); ++i) {
        WhatIfResult r;
        r.node = experiments[i].node;
        r.change = experiments[i].change;
        r.uph = uph[i];
        r.delta = uph[i] - rep.base_uph;
        r.delta_pct = rep.base_uph > 0 ? 100.0 * r.delta / rep.base_uph : 0;
        rep.results.push_back(std::move(r));
    }
    std::sort(rep.results.begin(), rep.results.end(), [](const auto& a, const auto& b) { return a.delta > b.delta; });

    std::ostringstream o;
    if (rep.results.empty()) {
        o << "No stations to tune.";
    } else if (rep.results.front().delta_pct < 1.0) {
        o << "No single local change moves throughput by more than 1%: the line is demand-limited or constrained elsewhere. "
          << "Try raising the arrival rate, or look at the system-wide structure.";
    } else {
        const auto& b = rep.results.front();
        o << "Best lever: " << b.change << " at '" << b.node << "' -> " << (b.delta >= 0 ? "+" : "") << fixed(b.delta, 1)
          << " units/hour (" << fixed(b.delta_pct, 1) << "%), measured over " << reps << " seeds with common random numbers.";
    }
    rep.recommendation = o.str();
    return rep;
}

json to_json(const WhatIfReport& r) {
    json res = json::array();
    for (const auto& x : r.results)
        res.push_back({{"node", x.node}, {"change", x.change}, {"uph", x.uph}, {"delta", x.delta}, {"delta_pct", x.delta_pct}});
    return {{"base_uph", r.base_uph}, {"reps", r.reps}, {"results", res}, {"recommendation", r.recommendation}};
}

}  // namespace factory
