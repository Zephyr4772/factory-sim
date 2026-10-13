#pragma once
// Generic discrete-event core: a min-heap future-event list ordered by (time, seq)
// and a clock that jumps from event to event. It knows nothing about factories.
#include <array>
#include <queue>
#include <vector>

#include "factory/types.hpp"

namespace factory {

class EventEngine {
public:
    explicit EventEngine(std::size_t reserve = 4096);

    // Schedules an event at absolute time `at` (clamped to now: no time travel).
    void schedule(Tick at, EventType type, NodeId node = kNoNode, std::uint32_t gen = 0,
                  PartId part = kNoPart);

    [[nodiscard]] bool empty() const noexcept { return heap_.empty(); }
    [[nodiscard]] std::size_t pending() const noexcept { return heap_.size(); }
    [[nodiscard]] std::size_t pending(EventType t) const noexcept {
        return pending_by_type_[static_cast<std::size_t>(t)];
    }
    [[nodiscard]] Tick now() const noexcept { return now_; }
    [[nodiscard]] std::uint64_t processed() const noexcept { return processed_; }

    // Pops the earliest event and advances the clock to it. False if empty.
    bool pop(Event& out);

private:
    struct Later {
        bool operator()(const Event& x, const Event& y) const noexcept {
            return x.time != y.time ? x.time > y.time : x.seq > y.seq;
        }
    };
    using Heap = std::priority_queue<Event, std::vector<Event>, Later>;
    static Heap make_heap(std::size_t reserve);

    Heap heap_;
    Tick now_ = 0;
    std::uint64_t next_seq_ = 0;
    std::uint64_t processed_ = 0;
    std::array<std::size_t, kEventTypeCount> pending_by_type_{};
};

}  // namespace factory
