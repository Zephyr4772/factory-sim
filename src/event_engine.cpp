#include "factory/event_engine.hpp"

#include <algorithm>

namespace factory {

EventEngine::Heap EventEngine::make_heap(std::size_t reserve) {
    std::vector<Event> storage;
    storage.reserve(reserve);
    return Heap(Later{}, std::move(storage));
}

EventEngine::EventEngine(std::size_t reserve) : heap_(make_heap(reserve)) {}

void EventEngine::schedule(Tick at, EventType type, NodeId node, std::uint32_t gen, PartId part) {
    Event e;
    e.time = std::max(at, now_);
    e.seq = next_seq_++;
    e.type = type;
    e.node = node;
    e.gen = gen;
    e.part = part;
    heap_.push(e);
    ++pending_by_type_[static_cast<std::size_t>(type)];
}

bool EventEngine::pop(Event& out) {
    if (heap_.empty()) return false;
    out = heap_.top();
    heap_.pop();
    --pending_by_type_[static_cast<std::size_t>(out.type)];
    now_ = out.time;
    ++processed_;
    return true;
}

}  // namespace factory
