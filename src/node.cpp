#include "factory/node.hpp"

#include <algorithm>
#include <stdexcept>

#include "factory/plant.hpp"

namespace factory {

bool RouteMatch::matches(const Part& p) const {
    if (type >= 0 && p.type != type) return false;
    if (quality_lt && !(p.quality < *quality_lt)) return false;
    if (quality_ge && !(p.quality >= *quality_ge)) return false;
    if (reworks_ge && !(p.reworks >= *reworks_ge)) return false;
    if (reworks_lt && !(p.reworks < *reworks_lt)) return false;
    if (priority_ge && !(p.priority >= *priority_ge)) return false;
    return true;
}

Node::Node(Plant& plant, std::string name)
    : plant_(&plant), name_(std::move(name)), route_rng_(plant.settings().seed, name_ + "/route") {}

Tick Node::now() const { return plant_->now(); }

RngStream Node::make_stream(std::string_view purpose) const {
    return RngStream(plant_->settings().seed, name_ + "/" + std::string(purpose));
}

void Node::set_state(NodeState s) {
    if (s == state_) return;
    stats_.time_in_state[static_cast<std::size_t>(state_)] += now() - since_;
    since_ = now();
    plant_->state_changed(state_, s);
    state_ = s;
    plant_->trace_state(id_, s);
}

NodeStats Node::stats_snapshot() const {
    NodeStats s = stats_;
    s.time_in_state[static_cast<std::size_t>(state_)] += now() - since_;
    return s;
}

void Node::reset_stats() {
    stats_ = NodeStats{};
    since_ = now();
    stats_.max_occupancy = units_inside();
}

void Node::integrate(Tick dt) {
    stats_.occupancy_area += static_cast<double>(units_inside()) * static_cast<double>(dt);
}

void Node::note_queue() {
    stats_.max_occupancy = std::max(stats_.max_occupancy, units_inside());
    plant_->trace_queue(id_, queue_length());
}

std::vector<NodeId> Node::take_waiters() { return std::exchange(waiters_, {}); }

void Node::unregister_waiting() {
    for (NodeId target : waiting_on_) {
        auto& w = plant_->node(target).waiters_;
        w.erase(std::remove(w.begin(), w.end(), id_), w.end());
    }
    waiting_on_.clear();
}

bool Node::edge_accepts(int edge_index, const Part& p) const {
    const Edge& e = edges_[static_cast<std::size_t>(edge_index)];
    return plant_->node(e.to).can_accept(p, e.bypass);
}

void Node::gather_candidates(const Part& p) {
    candidates_.clear();
    for (const RouteRule& rule : routing_.rules) {
        if (rule.match.matches(p) && !rule.edges.empty()) {
            candidates_ = rule.edges;
            return;
        }
    }
    for (std::size_t i = 0; i < edges_.size(); ++i) candidates_.push_back(static_cast<int>(i));
}

bool Node::try_forward(PartId pid) {
    if (edges_.empty()) throw std::logic_error("node '" + name_ + "' has no outgoing edges");
    const Part& p = plant_->part(pid);

    if (!sticky_) {
        if (auto forced = route_override(p)) {
            sticky_ = *forced;
        } else {
            gather_candidates(p);
            if (routing_.policy == RoutePolicy::Probabilistic) {
                // Draw once and stay: re-rolling while blocked would bias the split.
                std::vector<double> weights;
                weights.reserve(candidates_.size());
                for (int c : candidates_) weights.push_back(edges_[static_cast<std::size_t>(c)].weight);
                sticky_ = candidates_[route_rng_.pick(weights)];
            }
        }
    }

    int chosen = -1;
    if (sticky_) {
        if (edge_accepts(*sticky_, p)) chosen = *sticky_;
    } else {
        const std::size_t n = candidates_.size();
        switch (routing_.policy) {
            case RoutePolicy::FirstAvailable:
            case RoutePolicy::Probabilistic:
                for (int c : candidates_)
                    if (edge_accepts(c, p)) { chosen = c; break; }
                break;
            case RoutePolicy::RoundRobin:
                for (std::size_t k = 0; k < n; ++k) {
                    const std::size_t pos = (rr_cursor_ + k) % n;
                    if (edge_accepts(candidates_[pos], p)) {
                        chosen = candidates_[pos];
                        rr_cursor_ = pos + 1;
                        break;
                    }
                }
                break;
            case RoutePolicy::ShortestQueue: {
                int best_len = 0;
                for (int c : candidates_) {
                    if (!edge_accepts(c, p)) continue;
                    const int len = plant_->node(edges_[static_cast<std::size_t>(c)].to).queue_length();
                    if (chosen < 0 || len < best_len) { chosen = c; best_len = len; }
                }
                break;
            }
        }
    }

    if (chosen < 0) {
        unregister_waiting();
        auto wait_on = [&](int edge_index) {
            const NodeId t = edges_[static_cast<std::size_t>(edge_index)].to;
            auto& w = plant_->node(t).waiters_;
            if (std::find(w.begin(), w.end(), id_) == w.end()) w.push_back(id_);
            waiting_on_.push_back(t);
        };
        if (sticky_) wait_on(*sticky_);
        else for (int c : candidates_) wait_on(c);
        return false;
    }

    unregister_waiting();
    sticky_.reset();
    const Edge edge = edges_[static_cast<std::size_t>(chosen)];
    plant_->deliver(*this, edge, pid);
    return true;
}

}  // namespace factory
