#pragma once
// The extension point. Every pipeline element (built-in or custom) derives from Node.
//
// A node owns: an input side (can_accept / accept), a behaviour (on_event, ...), and an
// output side (routing over its outgoing edges + backpressure through try_forward).
// To write a custom node, derive from Node (or Machine), register it in the
// NodeRegistry, and it automatically appears in config files and in the Studio UI.
#include <array>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "factory/distribution.hpp"
#include "factory/rng.hpp"
#include "factory/types.hpp"

namespace factory {

class Plant;
using json = nlohmann::json;

enum class RoutePolicy { Probabilistic, RoundRobin, ShortestQueue, FirstAvailable };

struct Edge {
    NodeId to = kNoNode;
    double weight = 1.0;
    bool rework = false;  // traversing this edge increments Part::reworks
    bool bypass = false;  // target accepts the part even if its buffer is full
                          // (a "rework lane": breaks circular waits)
    std::string label;
};

// First-match routing rule: parts matching `match` may only go to `edges` (indices into
// Node::edges()); the node's policy then picks among them.
struct RouteMatch {
    int type = -1;  // part-type id, -1 = any
    std::optional<double> quality_lt, quality_ge;
    std::optional<int> reworks_ge, reworks_lt, priority_ge;
    [[nodiscard]] bool matches(const Part& p) const;
};
struct RouteRule {
    RouteMatch match;
    std::vector<int> edges;
};
struct Routing {
    RoutePolicy policy = RoutePolicy::Probabilistic;
    std::vector<RouteRule> rules;
};

struct NodeStats {
    std::array<Tick, kStateCount> time_in_state{};
    double occupancy_area = 0;  // integral of units_inside() over time (unit-ticks)
    int max_occupancy = 0;
    std::uint64_t processed_units = 0;
    std::uint64_t lost_units = 0;
    std::uint64_t failures = 0;
    Tick wait_sum = 0;  // total time parts spent waiting in this node's buffer
    std::uint64_t wait_count = 0;
};

class Node {
public:
    Node(Plant& plant, std::string name);
    virtual ~Node() = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;

    // ---- identity ----
    [[nodiscard]] NodeId id() const noexcept { return id_; }
    [[nodiscard]] const std::string& name() const noexcept { return name_; }
    [[nodiscard]] virtual std::string_view kind() const = 0;
    // Stations take part in bottleneck analysis (sources, sinks and conveyors do not).
    [[nodiscard]] virtual bool is_station() const { return false; }

    // ---- wiring (done by the loader) ----
    void set_id(NodeId id) noexcept { id_ = id; }
    void add_edge(Edge e) { edges_.push_back(std::move(e)); }
    [[nodiscard]] const std::vector<Edge>& edges() const noexcept { return edges_; }
    [[nodiscard]] Routing& routing() noexcept { return routing_; }
    [[nodiscard]] const Routing& routing() const noexcept { return routing_; }

    // ---- lifecycle ----
    virtual void resolve() {}  // after all nodes/edges exist: turn names into indices
    virtual void start() {}    // schedule initial events
    virtual void on_event(const Event&) {}
    virtual void on_space() {}  // a downstream we were blocked on freed a slot

    // ---- input side ----
    [[nodiscard]] virtual bool can_accept(const Part&, bool /*bypass*/) const { return false; }
    virtual void accept(PartId) {}
    [[nodiscard]] virtual int queue_length() const { return 0; }  // waiting parts only
    [[nodiscard]] virtual int capacity() const { return -1; }     // -1 = unbounded
    [[nodiscard]] virtual int units_inside() const { return 0; }  // for WIP statistics
    [[nodiscard]] virtual json describe() const { return json::object(); }
    // True if this node can still push *new* work into the line (sources only). The deadlock
    // detector uses it: a stalled line is only deadlocked if no fresh arrival could restart it.
    [[nodiscard]] virtual bool can_inject() const { return false; }

    // ---- state & statistics ----
    [[nodiscard]] NodeState state() const noexcept { return state_; }
    [[nodiscard]] NodeStats stats_snapshot() const;  // state times flushed to "now"
    void reset_stats();
    void integrate(Tick dt);

    // ---- backpressure bookkeeping (used by Plant) ----
    [[nodiscard]] std::vector<NodeId> take_waiters();
    void unregister_waiting();
    [[nodiscard]] const std::vector<NodeId>& waiting_on() const noexcept { return waiting_on_; }

protected:
    [[nodiscard]] Plant& plant() const noexcept { return *plant_; }
    [[nodiscard]] Tick now() const;
    void set_state(NodeState s);
    void note_queue();  // call after the buffer changed: updates max-occupancy and the trace
    [[nodiscard]] RngStream make_stream(std::string_view purpose) const;

    // Tries to hand `pid` to a downstream node. On failure the node registers itself with
    // every candidate target and will get on_space() when one of them frees a slot.
    [[nodiscard]] bool try_forward(PartId pid);

    // Gives up on forwarding the current part (e.g. it is being dropped): clears the
    // blocked-on registrations and the sticky routing choice.
    void abandon_forward() {
        unregister_waiting();
        sticky_.reset();
    }

    // Custom routing hook: return an edge index to force the destination.
    [[nodiscard]] virtual std::optional<int> route_override(const Part&) { return std::nullopt; }

    NodeStats stats_;

private:
    [[nodiscard]] bool edge_accepts(int edge_index, const Part& p) const;
    void gather_candidates(const Part& p);

    Plant* plant_;
    NodeId id_ = kNoNode;
    std::string name_;
    std::vector<Edge> edges_;
    Routing routing_;
    NodeState state_ = NodeState::Idle;
    Tick since_ = 0;
    RngStream route_rng_;
    std::size_t rr_cursor_ = 0;
    std::optional<int> sticky_;      // edge chosen for the part currently being forwarded
    std::vector<int> candidates_;    // reusable scratch
    std::vector<NodeId> waiters_;    // nodes blocked waiting for *us*
    std::vector<NodeId> waiting_on_; // nodes *we* are blocked waiting for
};

}  // namespace factory
