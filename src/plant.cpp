#include "factory/plant.hpp"

#include <algorithm>
#include <stdexcept>

namespace factory {

Plant::Plant(SimSettings settings) : settings_(settings) {
    parts_.reserve(1024);
    if (settings_.trace) trace_.reserve(65536);
}

Plant::~Plant() = default;

NodeId Plant::add_node(std::unique_ptr<Node> node) {
    const auto id = static_cast<NodeId>(nodes_.size());
    if (!by_name_.emplace(node->name(), id).second)
        throw std::invalid_argument("duplicate node id '" + node->name() + "'");
    node->set_id(id);
    nodes_.push_back(std::move(node));
    return id;
}

void Plant::connect(NodeId from, Edge e) { node(from).add_edge(std::move(e)); }

std::optional<NodeId> Plant::find(std::string_view name) const {
    auto it = by_name_.find(std::string(name));
    if (it == by_name_.end()) return std::nullopt;
    return it->second;
}

int Plant::type_id(std::string_view name) {
    auto it = type_ids_.find(std::string(name));
    if (it != type_ids_.end()) return it->second;
    const int id = static_cast<int>(type_names_.size());
    type_names_.emplace_back(name);
    type_ids_.emplace(std::string(name), id);
    return id;
}

void Plant::finalize() {
    if (finalized_) return;
    finalized_ = true;
    for (auto& n : nodes_) n->resolve();
    for (auto& n : nodes_) n->start();
    if (settings_.sample_interval_s > 0)
        engine_.schedule(to_ticks(settings_.sample_interval_s), EventType::Sample);
    if (settings_.warmup_s > 0)
        engine_.schedule(to_ticks(settings_.warmup_s), EventType::WarmupEnd);
    end_time_ = to_ticks(settings_.duration_s);
    engine_.schedule(end_time_, EventType::SimulationEnd);
}

void Plant::run() {
    finalize();
    Event ev;
    while (!stopped_ && engine_.pop(ev)) {
        advance_clock(ev.time);
        dispatch(ev);
        if (!stopped_) check_deadlock();
    }
    end_time_ = engine_.now();
}

void Plant::advance_clock(Tick to) {
    const Tick dt = to - last_advance_;
    if (dt <= 0) return;
    const double area = static_cast<double>(wip_units_) * static_cast<double>(dt);
    total_.wip_area += area;
    window_.wip_area += area;
    for (auto& n : nodes_) n->integrate(dt);
    last_advance_ = to;
}

void Plant::dispatch(const Event& ev) {
    switch (ev.type) {
        case EventType::SimulationEnd:
            stopped_ = true;
            break;
        case EventType::WarmupEnd:
            window_ = Counters{};
            window_start_ = now();
            for (auto& n : nodes_) n->reset_stats();
            break;
        case EventType::Sample:
            samples_.push_back({now(), wip_units_, total_.completed});
            engine_.schedule(now() + to_ticks(settings_.sample_interval_s), EventType::Sample);
            break;
        case EventType::BufferSlotFreed: {
            // Wake blocked upstreams in the order they got blocked. Each retries; those
            // that still cannot move re-register themselves.
            for (NodeId w : node(ev.node).take_waiters()) {
                Node& waiter = node(w);
                waiter.unregister_waiting();
                waiter.on_space();
            }
            break;
        }
        default:
            node(ev.node).on_event(ev);
            break;
    }
}

void Plant::schedule(Tick delay, EventType t, NodeId n, std::uint32_t gen, PartId part) {
    engine_.schedule(now() + delay, t, n, gen, part);
}

PartId Plant::create_part(int type, double quality, int priority) {
    PartId id;
    if (!free_parts_.empty()) {
        id = free_parts_.back();
        free_parts_.pop_back();
    } else {
        id = static_cast<PartId>(parts_.size());
        parts_.emplace_back();
    }
    Part& p = parts_[id];
    p = Part{};
    p.id = id;
    p.type = type;
    p.born_sum = now();
    p.quality = quality;
    p.priority = priority;
    p.alive = true;
    ++total_.created;
    ++window_.created;
    ++wip_units_;
    return id;
}

PartId Plant::merge(std::span<const PartId> inputs, int out_type) {
    int units = 0, reworks = 0, priority = 0;
    Tick born_sum = 0;
    double quality = 1.0;
    for (PartId pid : inputs) {
        const Part& p = parts_[pid];
        units += p.units;
        born_sum += p.born_sum;
        quality = std::min(quality, p.quality);
        reworks = std::max(reworks, p.reworks);
        priority = std::max(priority, p.priority);
    }
    for (PartId pid : inputs) {
        parts_[pid].alive = false;
        free_parts_.push_back(pid);
    }
    PartId id;
    if (!free_parts_.empty()) {
        id = free_parts_.back();
        free_parts_.pop_back();
    } else {
        id = static_cast<PartId>(parts_.size());
        parts_.emplace_back();
    }
    Part& out = parts_[id];
    out = Part{};
    out.id = id;
    out.type = out_type;
    out.units = units;
    out.born_sum = born_sum;
    out.quality = quality;
    out.reworks = reworks;
    out.priority = priority;
    out.alive = true;
    return id;  // WIP is unchanged: units are conserved by a merge
}

void Plant::push_trace(TraceKind k, std::int32_t a, std::int32_t b, std::int32_t c) {
    if (!settings_.trace) return;
    if (trace_.size() >= settings_.trace_limit) {
        trace_truncated_ = true;
        return;
    }
    trace_.push_back({now(), k, a, b, c});
}

void Plant::trace_state(NodeId n, NodeState s) { push_trace(TraceKind::State, n, static_cast<std::int32_t>(s), 0); }
void Plant::trace_queue(NodeId n, int len) { push_trace(TraceKind::Queue, n, len, 0); }

void Plant::deliver(Node& from, const Edge& e, PartId pid) {
    if (e.rework) ++parts_[pid].reworks;
    push_trace(TraceKind::Move, from.id(), e.to, parts_[pid].type);
    node(e.to).accept(pid);
}

void Plant::retire(PartId pid, Node& /*at*/, bool completed) {
    const Part& p = parts_[pid];
    const auto units = static_cast<std::uint64_t>(p.units);
    const Tick sojourn = static_cast<Tick>(p.units) * now() - p.born_sum;
    wip_units_ -= p.units;
    for (Counters* c : {&total_, &window_}) {
        (completed ? c->completed : c->scrapped) += units;
        c->departed += units;
        c->sojourn += sojourn;
    }
    parts_[pid].alive = false;
    free_parts_.push_back(pid);
}

void Plant::drop(PartId pid, Node& at, bool at_source) {
    const Part& p = parts_[pid];
    const auto units = static_cast<std::uint64_t>(p.units);
    const Tick sojourn = static_cast<Tick>(p.units) * now() - p.born_sum;
    wip_units_ -= p.units;
    for (Counters* c : {&total_, &window_}) {
        c->lost += units;
        if (!at_source) {  // it did enter the system, so it counts for Little's Law
            c->departed += units;
            c->sojourn += sojourn;
        }
    }
    push_trace(TraceKind::Lost, at.id(), 0, 0);
    parts_[pid].alive = false;
    free_parts_.push_back(pid);
}

void Plant::signal_space(NodeId id) { engine_.schedule(now(), EventType::BufferSlotFreed, id); }

void Plant::state_changed(NodeState from, NodeState to) {
    if (from == NodeState::Blocked) --blocked_count_;
    if (to == NodeState::Blocked) ++blocked_count_;
}

void Plant::check_deadlock() {
    if (deadlock_ || blocked_count_ <= 0) return;
    // Only things that can unblock someone count as "progress still possible".
    // Arrivals, failures and samples cannot resolve a circular wait.
    if (engine_.pending(EventType::ProcessingCompleted) || engine_.pending(EventType::MachineRepaired) ||
        engine_.pending(EventType::TransitCompleted) || engine_.pending(EventType::Timer) ||
        engine_.pending(EventType::BufferSlotFreed))
        return;
    // A new arrival may still restart a starved line (e.g. an assembler waiting for a
    // missing part type). Only if no source can inject into the line is it a deadlock.
    for (const auto& n : nodes_)
        if (n->can_inject()) return;

    DeadlockInfo info;
    info.at = now();
    std::vector<NodeId> blocked;
    for (const auto& n : nodes_)
        if (n->state() == NodeState::Blocked) {
            info.blocked.push_back(n->name());
            blocked.push_back(n->id());
        }
    // Follow "blocked on" links until a node repeats: that is the wait-for cycle.
    std::vector<NodeId> path;
    NodeId cur = blocked.front();
    while (std::find(path.begin(), path.end(), cur) == path.end()) {
        path.push_back(cur);
        NodeId next = kNoNode;
        for (NodeId t : node(cur).waiting_on())
            if (node(t).state() == NodeState::Blocked) { next = t; break; }
        if (next == kNoNode) { path.clear(); break; }
        cur = next;
    }
    if (!path.empty()) {
        auto start = std::find(path.begin(), path.end(), cur);
        for (auto it = start; it != path.end(); ++it) info.cycle += node(*it).name() + " -> ";
        info.cycle += node(cur).name();
    }
    deadlock_ = std::move(info);
    if (settings_.stop_on_deadlock) stopped_ = true;
}

}  // namespace factory
