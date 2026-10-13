#pragma once
// Core vocabulary shared by every module: time, ids, node states, events, parts.
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace factory {

// Simulated time is integer microseconds: no drift, exact ties.
using Tick = std::int64_t;
inline constexpr Tick kTicksPerSecond = 1'000'000;

[[nodiscard]] inline Tick to_ticks(double seconds) noexcept {
    return static_cast<Tick>(std::llround(seconds * static_cast<double>(kTicksPerSecond)));
}
[[nodiscard]] inline double to_seconds(Tick t) noexcept {
    return static_cast<double>(t) / static_cast<double>(kTicksPerSecond);
}

using NodeId = std::int32_t;
inline constexpr NodeId kNoNode = -1;
using PartId = std::uint32_t;
inline constexpr PartId kNoPart = 0xFFFFFFFFu;

// IDLE doubles as "STARVED": the node is ready but has nothing to do.
enum class NodeState : std::uint8_t { Idle, Busy, Blocked, Down };
inline constexpr std::size_t kStateCount = 4;

[[nodiscard]] constexpr const char* to_string(NodeState s) noexcept {
    switch (s) {
        case NodeState::Idle: return "IDLE";
        case NodeState::Busy: return "BUSY";
        case NodeState::Blocked: return "BLOCKED";
        case NodeState::Down: return "DOWN";
    }
    return "?";
}

enum class EventType : std::uint8_t {
    PartSpawned,          // a source creates a part
    ProcessingCompleted,  // a node finished working on its job
    BufferSlotFreed,      // a node pulled from its buffer; blocked upstreams may retry
    MachineFailed,        // MTBF fired
    MachineRepaired,      // MTTR elapsed
    TransitCompleted,     // a part reached the end of a conveyor/delay
    Timer,                // node-private wake-up (e.g. batch timeout)
    Sample,               // periodic telemetry snapshot
    WarmupEnd,            // statistics window starts
    SimulationEnd         // hard stop
};
inline constexpr std::size_t kEventTypeCount = 10;

[[nodiscard]] constexpr const char* to_string(EventType t) noexcept {
    switch (t) {
        case EventType::PartSpawned: return "PART_SPAWNED";
        case EventType::ProcessingCompleted: return "PROCESSING_COMPLETED";
        case EventType::BufferSlotFreed: return "BUFFER_SLOT_FREED";
        case EventType::MachineFailed: return "MACHINE_FAILED";
        case EventType::MachineRepaired: return "MACHINE_REPAIRED";
        case EventType::TransitCompleted: return "TRANSIT_COMPLETED";
        case EventType::Timer: return "TIMER";
        case EventType::Sample: return "SAMPLE";
        case EventType::WarmupEnd: return "WARMUP_END";
        case EventType::SimulationEnd: return "SIMULATION_END";
    }
    return "?";
}

// `gen` is a generation token: handlers drop events whose gen no longer matches
// (lazy cancellation, since a binary heap cannot remove arbitrary entries).
struct Event {
    Tick time = 0;
    std::uint64_t seq = 0;  // tie-breaker: equal times fire in scheduling order
    EventType type = EventType::SimulationEnd;
    NodeId node = kNoNode;
    std::uint32_t gen = 0;
    PartId part = kNoPart;
};

// A part may represent several units after batching/assembly. `born_sum` is the sum
// of the birth times of its units, which keeps cycle-time and Little's-Law accounting
// exact through merges.
struct Part {
    PartId id = kNoPart;
    int type = 0;
    int units = 1;
    Tick born_sum = 0;
    double quality = 1.0;
    int reworks = 0;
    int priority = 0;
    bool alive = false;
};

}  // namespace factory
