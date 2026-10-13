#pragma once
// The plant owns the nodes, the event engine, the part pool and all global counters,
// and runs the simulation loop.
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "factory/event_engine.hpp"
#include "factory/node.hpp"

namespace factory {

struct SimSettings {
    double duration_s = 3600.0;
    std::uint64_t seed = 42;
    double warmup_s = 0.0;
    double sample_interval_s = 60.0;  // 0 disables telemetry sampling
    bool stop_on_deadlock = true;
    bool trace = false;               // record a replayable trace (for the Studio)
    std::size_t trace_limit = 250000;
};

// Unit counters. `Plant::total()` never resets; `Plant::window()` restarts at warm-up end.
struct Counters {
    std::uint64_t created = 0;
    std::uint64_t lost = 0;       // refused by a full buffer / drop policy
    std::uint64_t completed = 0;  // reached a "completed" sink
    std::uint64_t scrapped = 0;   // reached a "scrapped" sink or was destroyed by a failure
    std::uint64_t departed = 0;   // left the system after entering it (completed+scrapped+downstream drops)
    Tick sojourn = 0;             // total unit-time-in-system of departed units
    double wip_area = 0;          // integral of WIP over time (unit-ticks)
};

enum class TraceKind : std::uint8_t { State = 0, Queue = 1, Move = 2, Lost = 3 };
struct TraceEvent {
    Tick t;
    TraceKind kind;
    std::int32_t a, b, c;  // State: node,state | Queue: node,len | Move: from,to,type | Lost: node
};
struct Sample {
    Tick t;
    std::int64_t wip;
    std::uint64_t completed;
};
struct DeadlockInfo {
    Tick at = 0;
    std::vector<std::string> blocked;  // nodes stuck in BLOCKED
    std::string cycle;                 // e.g. "inspect -> assembly -> inspect"
};

class Plant {
public:
    explicit Plant(SimSettings settings);
    ~Plant();
    Plant(const Plant&) = delete;
    Plant& operator=(const Plant&) = delete;

    // ---- construction ----
    NodeId add_node(std::unique_ptr<Node> node);
    void connect(NodeId from, Edge e);
    void finalize();  // resolve names, start nodes, schedule control events
    void run();       // run until duration, or deadlock

    [[nodiscard]] Node& node(NodeId id) { return *nodes_[static_cast<std::size_t>(id)]; }
    [[nodiscard]] const Node& node(NodeId id) const { return *nodes_[static_cast<std::size_t>(id)]; }
    [[nodiscard]] std::size_t node_count() const noexcept { return nodes_.size(); }
    [[nodiscard]] std::optional<NodeId> find(std::string_view name) const;

    int type_id(std::string_view name);  // interns part-type names
    [[nodiscard]] const std::vector<std::string>& type_names() const noexcept { return type_names_; }

    // ---- services used by nodes ----
    [[nodiscard]] Tick now() const noexcept { return engine_.now(); }
    void schedule(Tick delay, EventType t, NodeId node, std::uint32_t gen = 0, PartId part = kNoPart);
    // Returned references into the pool are invalidated by create_part/merge.
    PartId create_part(int type, double quality, int priority);
    [[nodiscard]] Part& part(PartId id) { return parts_[id]; }
    [[nodiscard]] const Part& part(PartId id) const { return parts_[id]; }
    PartId merge(std::span<const PartId> inputs, int out_type);
    void deliver(Node& from, const Edge& e, PartId pid);
    void retire(PartId pid, Node& at, bool completed);
    void drop(PartId pid, Node& at, bool at_source);
    void signal_space(NodeId id);
    void state_changed(NodeState from, NodeState to);
    void trace_state(NodeId n, NodeState s);
    void trace_queue(NodeId n, int len);

    // ---- results ----
    [[nodiscard]] const SimSettings& settings() const noexcept { return settings_; }
    [[nodiscard]] const EventEngine& engine() const noexcept { return engine_; }
    [[nodiscard]] const Counters& total() const noexcept { return total_; }
    [[nodiscard]] const Counters& window() const noexcept { return window_; }
    [[nodiscard]] std::int64_t wip_units() const noexcept { return wip_units_; }
    [[nodiscard]] Tick window_start() const noexcept { return window_start_; }
    [[nodiscard]] Tick end_time() const noexcept { return end_time_; }
    [[nodiscard]] const std::vector<Sample>& samples() const noexcept { return samples_; }
    [[nodiscard]] const std::vector<TraceEvent>& trace() const noexcept { return trace_; }
    [[nodiscard]] bool trace_truncated() const noexcept { return trace_truncated_; }
    [[nodiscard]] const std::optional<DeadlockInfo>& deadlock() const noexcept { return deadlock_; }
    [[nodiscard]] std::vector<std::string>& warnings() noexcept { return warnings_; }
    [[nodiscard]] const std::vector<std::string>& warnings() const noexcept { return warnings_; }

private:
    void dispatch(const Event& ev);
    void advance_clock(Tick to);
    void check_deadlock();
    void push_trace(TraceKind k, std::int32_t a, std::int32_t b, std::int32_t c);

    SimSettings settings_;
    EventEngine engine_;
    std::vector<std::unique_ptr<Node>> nodes_;
    std::unordered_map<std::string, NodeId> by_name_;
    std::vector<std::string> type_names_;
    std::unordered_map<std::string, int> type_ids_;

    std::vector<Part> parts_;
    std::vector<PartId> free_parts_;

    Counters total_, window_;
    std::int64_t wip_units_ = 0;
    Tick last_advance_ = 0;
    Tick window_start_ = 0;
    Tick end_time_ = 0;
    int blocked_count_ = 0;
    bool stopped_ = false;
    bool finalized_ = false;

    std::vector<Sample> samples_;
    std::vector<TraceEvent> trace_;
    bool trace_truncated_ = false;
    std::optional<DeadlockInfo> deadlock_;
    std::vector<std::string> warnings_;
};

}  // namespace factory
