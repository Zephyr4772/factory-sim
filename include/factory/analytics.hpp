#pragma once
// Everything computed *after* a run: KPI summary, Little's Law, bottleneck detection
// (Theory of Constraints), replication statistics, measured what-if experiments, and the
// text/CSV/JSON/trace exporters.
#include <optional>
#include <string>
#include <vector>

#include "factory/config_loader.hpp"
#include "factory/plant.hpp"

namespace factory {

struct NodeReport {
    std::string id, kind;
    bool station = false;
    double pct[kStateCount] = {};  // share of the measured window in IDLE/BUSY/BLOCKED/DOWN
    double avg_occupancy = 0;      // time-average units inside
    int max_occupancy = 0;
    int capacity = -1;
    std::uint64_t processed_units = 0, lost_units = 0, failures = 0;
    double avg_wait_s = 0;
    double mtbf_s = 0;
    double bottleneck_index = 0;  // (busy% + down%) + a*avg(downstream starved%) + b*avg(upstream blocked%)
};

struct LittleLaw {
    double L = 0;       // time-average units in the system
    double lambda = 0;  // departure rate (units/s)
    double W = 0;       // mean time in system of departed units (s)
    double gap_pct = 0; // |L - lambda*W| / L: non-zero only through units still in flight at the cut-off
};

struct Summary {
    double window_s = 0;
    Counters window, total;
    std::int64_t wip_end = 0;
    double uph = 0;      // completed units per hour
    double avg_wip = 0;
    double cycle_time_s = 0;
    double lost_pct = 0;  // of created units
    bool conservation_ok = true;
    bool demand_limited = false;  // (almost) nothing is lost: the line keeps up with arrivals
    LittleLaw little;
    std::vector<NodeReport> nodes;
    std::string bottleneck;  // station with the highest bottleneck index
    double sources_blocked_pct = 0;  // worst source: share of time held back by a full line
    std::string diagnosis;
    std::optional<DeadlockInfo> deadlock;
    std::vector<std::string> warnings;
    std::vector<Sample> samples;
};

[[nodiscard]] Summary summarize(const Plant& plant);
[[nodiscard]] json to_json(const Summary& s);
[[nodiscard]] std::string to_ascii(const Summary& s);
[[nodiscard]] std::string samples_csv(const Plant& plant);
[[nodiscard]] json trace_to_json(const Plant& plant);  // compact event list for the Studio replay

// One full simulation from a config (seed/duration overridable).
[[nodiscard]] Summary run_once(const json& config, const NodeRegistry& registry, const BuildOptions& options = {});

struct Stat {
    double mean = 0, sd = 0, ci95 = 0;
};
struct ReplicationReport {
    int n = 0;
    Stat uph, wip, cycle_time_s, lost_pct;
    int deadlocks = 0;
};
[[nodiscard]] ReplicationReport replicate(const json& config, const NodeRegistry& registry, int reps);
[[nodiscard]] json to_json(const ReplicationReport& r);

struct WhatIfResult {
    std::string node, change;
    double uph = 0, delta = 0, delta_pct = 0;
};
struct WhatIfReport {
    double base_uph = 0;
    int reps = 0;
    std::vector<WhatIfResult> results;  // best first
    std::string recommendation;
};
// Re-runs the plant with candidate improvements at the likeliest bottlenecks, using the
// same seeds (common random numbers), and reports the *measured* throughput change.
[[nodiscard]] WhatIfReport what_if(const json& config, const NodeRegistry& registry, int reps = 3);
[[nodiscard]] json to_json(const WhatIfReport& r);

}  // namespace factory
