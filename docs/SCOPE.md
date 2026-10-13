# Factory Line DES: scope, review, and recommended changes

Companion visual: [visual.html](visual.html). Open it in a browser. It runs a miniature of this design: the same events, blocking rules and routing.

## 1. What the project is

A C++20 discrete-event simulator of an assembly plant. Parts arrive, queue in bounded buffers, are processed by stations that can fail, and are routed serially, in parallel, or into rework and scrap loops. The output is KPIs and a bottleneck diagnosis.

Core idea: simulated time jumps from event to event. All behaviour comes from five event handlers acting on a plant state and scheduling further events.

```
config.json ─► config_loader ─► topology (graph + routing) ─┐
                                                           ▼
 RNG streams ─► distributions ─► workstations ◄──► event_engine (FEL + clock)
                                      │
                                      └─► analytics (observer: state-time, WIP, Little, bottleneck)
```

## 2. Where the spec is sound

- Min-heap event list, jump clock, five event types plus `SIMULATION_END`: the standard design.
- Blocking-after-service (the part stays in the machine until downstream has room) is the right model for bounded buffers.
- Separating topology, workstation and analytics is sensible.
- Seeded `mt19937_64`, FetchContent dependencies, strict warnings, conservation test: all good.

## 3. Problems in the spec (fix before coding)

| # | Issue | Why it bites | Recommendation |
|---|---|---|---|
| 1 | **Rework loops can deadlock.** Inspection blocked on a full assembly buffer, assembly blocked on a full inspection buffer. | I hit this in the visual at a 14 s arrival gap: the line froze after ~77 parts. | Give rework a bypass or dedicated lane. Add a deadlock detector (no BUSY, no DOWN, ≥1 BLOCKED, FEL has only arrivals) that reports the wait-for cycle. Validate configs for cycles with no slack. |
| 2 | **Stale events.** A breakdown mid-cycle leaves a `PROCESSING_COMPLETED` in the heap, and `std::priority_queue` cannot remove it. | The machine would "finish" a part while DOWN. | Give each machine a generation counter and stamp it into events. Drop an event on mismatch (lazy deletion). The visual does this. |
| 3 | **Non-deterministic ties.** `greater<Event>` on timestamp alone leaves equal-time order unspecified. | Different stdlib heaps give different runs, so the "repeatable" claim fails. | Order by `(time, seq)` with a monotonically increasing sequence number. |
| 4 | **`double` clock.** | Accumulated drift, and equality ties become fragile. | Use `int64` ticks (microseconds). Convert at the config boundary. |
| 5 | **One shared `mt19937_64`.** | Adding a station reshuffles every other station's random draws. Cannot compare scenarios fairly. | One RNG stream per station and purpose (process, failure, routing, arrivals), seeded by splitting the master seed (`splitmix64`). This gives common random numbers for what-if comparisons. |
| 6 | **`std::normal_distribution` and `std::exponential_distribution` differ across stdlibs.** | Same seed gives different results on GCC, Clang and MSVC, contradicting the Linux/Windows/macOS goal. | Sample from the raw 64-bit stream yourself (inverse CDF, Box-Muller). Raw `mt19937_64` output is portable. |
| 7 | **`std::format` is not in GCC 12.** | Spec lists GCC 12+ and `std::format`. | Use `<iostream>` or `<iomanip>`, or fetch `{fmt}`. |
| 8 | **Truncated normal is vague.** | Clamping at 0 biases the mean. | Resample until positive, or use a log-normal. Document the choice. |
| 9 | **Buffer capacity is ambiguous.** Does the part in the machine count? | Changes blocking behaviour and Little's Law. | Define: capacity counts waiting parts only, and the machine holds one extra. State it in the README. |
| 10 | **"Notify upstream" is under-specified.** | Broadcasting wakes everyone and is unfair. | Keep a FIFO wait list per destination group. On a freed slot, release the longest-blocked upstream that can actually fit. Parallel groups need "blocked on group". |
| 11 | **Failure clock is unspecified.** | Time-based and operation-based MTBF give different downtime for the same numbers. | Make it a per-station option. Default to operating time (an idle machine does not wear out), and document it. |
| 12 | **Arrival into a full entry buffer is undefined.** | Conservation test breaks silently. | Count rejected/lost arrivals as a metric. Conservation: `spawned = rejected + completed + scrapped + WIP`. |
| 13 | **Little's Law "error margin" will be non-zero** for finite runs. | Looks like a bug. | Compute `L` from the time-integral of WIP and `W` from sojourn times (including in-flight parts) and explain the boundary gap. Add a warm-up cut so the gap shrinks. |
| 14 | **Bottleneck index double counts.** | A blocked-upstream signal is already implied by high utilization downstream. Weights α, β are unjustified. | Keep the index as a heuristic, but add the active-period method (the station with the longest uninterrupted busy periods is the bottleneck) and, more importantly, the what-if runs in section 4. |

## 4. Recommended additions (in priority order)

1. **Automated what-if analysis.** The spec recommends "increase buffer by N or cut MTTR by M%". The simulator can *measure* that: rerun with +1 buffer, −20% MTTR and +1 parallel machine at the bottleneck, using common random numbers, and report the real UPH delta. This replaces guessing.
2. **Replications with confidence intervals.** One 10-hour run is one sample. Run K seeds in parallel threads (`std::jthread`), then report mean ± 95% CI for UPH and WIP. Add a warm-up period parameter.
3. **Analytic validation tests.** These beat hand-built cases:
   - M/M/1: `L = ρ/(1−ρ)`
   - M/M/1/K: blocking probability matches the closed form
   - Two-station tandem with an infinite buffer: throughput equals the slower station
   - Zero-variance deterministic line: exact hand-computed completion times
   - Golden-file test: the same seed gives byte-identical CSV
4. **Event trace output** (JSONL: time, type, station, state before/after). It debugs the simulator and can drive a web replay like the visual.
5. **Config validation with line/field errors.** Check probabilities sum to 1, ids exist, the entry station exists, every station reaches a sink, and there is no zero-slack cycle.
6. **Product mix and setup times.** Parts carry a type, stations have per-type process times and changeover time. This is the main realism gap, and it is cheap if `Part` has a `type` field from day one.
7. **Scheduled maintenance** (planned downtime) alongside random failures, plus an optional **batching** station.
8. **Machine-readable results** (`results.json`) beside the ASCII table, so scripts can consume runs.

## 5. Architecture adjustments

- **Keep the engine generic.** `EventEngine` knows only `(time, seq, payload)` and a handler. All plant logic lives in a `Plant` model. The engine can then be unit tested alone, which the spec's `test_engine.cpp` implies.
- **Events as `std::variant`** of small structs (`PartSpawned`, `ProcessingCompleted{station, gen}`, …) with `std::visit`. No virtual calls or allocation in the hot loop. Reserve the heap's backing `vector` up front.
- **Parts live in a pool** (`std::vector<Part>` plus indices). Queues hold `PartId`, not parts, so moves are cheap and WIP counting is trivial.
- **Stations as indices into a flat vector**, not pointers. Topology stores adjacency as ids.
- **Analytics as an observer.** Stations call `on_state_change(station, old, new, t)` and `on_wip_change(...)`. Analytics integrates time-in-state exactly and never polls.
- **Extra files** beyond the spec: `rng.hpp` (stream splitting, samplers), `distribution.hpp` (a `concept Sampler`), `trace.hpp`.
- **Suggested milestones.** (1) engine plus ordering tests. (2) single station plus M/M/1 validation. (3) buffers, blocking and wait lists. (4) failures and generation counters. (5) topology, parallel and rework, and deadlock detection. (6) config loader. (7) analytics, bottleneck and what-if. (8) replications, README, CLI.

## 6. Open decisions for you

1. **Rework deadlock policy.** Bypass lane (my default), dedicated rework buffer, or fail the run with a diagnosis?
2. **Failure clock.** Operating-time (my default) or wall-clock?
3. **Scope.** Build the spec as written plus section 3 fixes, or also take on what-if analysis and replications (items 1 and 2 above), which I recommend?
4. **Time representation.** `int64` microsecond ticks (my default) or `double` seconds as the spec allows?
5. **Dependencies.** GoogleTest, or the lighter doctest? Both are fetched, and doctest compiles faster.

## 7. Decisions taken in the build

All recommendations above were adopted, plus the scope extension requested afterwards (custom nodes and a pipeline sandbox):

1. **Rework deadlock**: per-edge `bypass` lane, a loader warning for slack-free loops, and runtime detection that reports the wait-for cycle.
2. **Failure clock**: `failure_clock` is a per-machine option, default `operating`.
3. **Scope**: spec + section 3 fixes + what-if analysis + replications with confidence intervals, plus the node registry, node templates, custom C++ nodes and Factory Studio.
4. **Time**: `int64` microsecond ticks.
5. **Test framework**: doctest (header-only, fetched by CMake).

Two things changed from the plan after measuring:
* The assembler's capacity is **per part type**. A shared buffer let a flood of one type permanently starve the kit of another.
* The bottleneck index counts DOWN as utilisation (a machine under repair still consumes capacity). A "longest active period" detector was tried and removed: it mislabels low-utilisation stations with long single jobs. The what-if analysis is the arbiter.

See `README.md` for usage.
