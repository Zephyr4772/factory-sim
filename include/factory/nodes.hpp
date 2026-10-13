#pragma once
// Built-in node types. Machine is the workhorse: Batcher, Assembler and the custom
// QualityGate (src/custom_nodes.cpp) are small subclasses that override a few hooks.
#include <deque>
#include <map>
#include <memory>
#include <vector>

#include "factory/node.hpp"
#include "factory/params.hpp"
#include "factory/plant.hpp"

namespace factory {

enum class Discipline { Fifo, Lifo, Priority };
enum class FailureClock { Operating, Wall };          // MTBF counts busy time vs. calendar time
enum class FailurePolicy { Resume, Restart, Scrap };  // what happens to the part mid-cycle

struct MachineConfig {
    int capacity = -1;  // waiting parts only; the machine itself holds one more. -1 = unbounded
    Discipline discipline = Discipline::Fifo;
    bool drop_when_full = false;  // false: block upstream (backpressure)
    Distribution process = Distribution::constant(1.0);
    std::map<int, Distribution> process_by_type;  // per part-type override
    std::optional<Distribution> setup;            // changeover when the part type changes
    double mtbf_s = 0.0;                          // 0 = never fails
    double mttr_s = 0.0;
    FailureClock clock = FailureClock::Operating;
    FailurePolicy on_failure = FailurePolicy::Resume;
    double process_scale = 1.0;  // tuning knobs used by what-if analysis
    double repair_scale = 1.0;
};

class Machine : public Node {
public:
    Machine(Plant& plant, std::string name, MachineConfig cfg);

    [[nodiscard]] std::string_view kind() const override { return "machine"; }
    [[nodiscard]] bool is_station() const override { return true; }
    void start() override;
    void on_event(const Event& ev) override;
    void on_space() override;
    [[nodiscard]] bool can_accept(const Part& p, bool bypass) const override;
    void accept(PartId pid) override;
    [[nodiscard]] int queue_length() const override { return static_cast<int>(queue_.size()); }
    [[nodiscard]] int capacity() const override { return cfg_.capacity; }
    [[nodiscard]] int units_inside() const override { return queued_units_ + job_units_; }
    [[nodiscard]] json describe() const override;

protected:
    struct Queued {
        PartId id;
        Tick enter;
    };

    // ---- customisation hooks ----
    // Remove the parts for the next job from queue_ (use take_at) and return true.
    [[nodiscard]] virtual bool pick_work(std::vector<PartId>& out);
    // Turn the finished job's inputs into the single output part (default: pass through).
    [[nodiscard]] virtual PartId make_output(std::vector<PartId>& inputs) { return inputs.front(); }
    virtual void on_service_done(PartId /*output*/) {}
    virtual void after_enqueue() {}
    virtual void on_timer(std::uint32_t /*gen*/) {}

    PartId take_at(std::size_t index);
    void try_start();
    [[nodiscard]] const MachineConfig& config() const noexcept { return cfg_; }

    std::deque<Queued> queue_;

private:
    [[nodiscard]] std::size_t select_index() const;
    [[nodiscard]] bool full() const;
    void begin_busy(Tick duration);
    void leave_busy();
    void push_out();
    void on_complete(std::uint32_t gen);
    void on_failure(std::uint32_t gen);
    void on_repaired();
    [[nodiscard]] double service_seconds(const std::vector<PartId>& inputs);

    MachineConfig cfg_;
    RngStream rng_proc_, rng_setup_, rng_fail_, rng_repair_;
    std::vector<PartId> inputs_;  // parts of the job in progress
    PartId job_part_ = kNoPart;   // finished output waiting to leave
    int queued_units_ = 0;
    int job_units_ = 0;
    Tick remaining_ = 0, end_at_ = 0, busy_since_ = 0, job_duration_ = 0, ttf_remaining_ = 0;
    std::uint32_t gen_ = 0, fail_gen_ = 0;
    int last_type_ = -1;
};

// Collects `batch_size` parts (or whatever is waiting after `timeout_s`) into one batch.
struct BatcherConfig {
    MachineConfig machine;
    int batch_size = 4;
    double timeout_s = 0.0;
    std::optional<int> output_type;
};
class Batcher final : public Machine {
public:
    Batcher(Plant& plant, std::string name, BatcherConfig cfg);
    [[nodiscard]] std::string_view kind() const override { return "batcher"; }

protected:
    bool pick_work(std::vector<PartId>& out) override;
    PartId make_output(std::vector<PartId>& inputs) override;
    void after_enqueue() override;
    void on_timer(std::uint32_t gen) override;

private:
    void arm_timer();
    BatcherConfig bcfg_;
    std::uint32_t timer_gen_ = 0;
};

// Waits until its buffer holds a complete kit (e.g. 1 body + 4 wheels) and merges it.
struct AssemblerConfig {
    MachineConfig machine;
    std::vector<std::pair<int, int>> kit;  // part type -> count
    std::optional<int> output_type;
};
class Assembler final : public Machine {
public:
    Assembler(Plant& plant, std::string name, AssemblerConfig cfg);
    [[nodiscard]] std::string_view kind() const override { return "assembler"; }
    // Capacity is per part type: a flood of one type can never starve the kit of another.
    [[nodiscard]] bool can_accept(const Part& p, bool bypass) const override;

protected:
    bool pick_work(std::vector<PartId>& out) override;
    PartId make_output(std::vector<PartId>& inputs) override;

private:
    AssemblerConfig acfg_;
};

// A conveyor / transit step: unlimited parallel transit, no processing, order-preserving exit.
class DelayNode final : public Node {
public:
    DelayNode(Plant& plant, std::string name, Distribution transit, int capacity);
    [[nodiscard]] std::string_view kind() const override { return "delay"; }
    void on_event(const Event& ev) override;
    void on_space() override { pump(); }
    [[nodiscard]] bool can_accept(const Part&, bool bypass) const override;
    void accept(PartId pid) override;
    [[nodiscard]] int queue_length() const override { return in_transit_ + static_cast<int>(ready_.size()); }
    [[nodiscard]] int capacity() const override { return capacity_; }
    [[nodiscard]] int units_inside() const override { return units_; }

private:
    void pump();
    Distribution transit_;
    int capacity_;
    RngStream rng_;
    std::deque<PartId> ready_;
    int in_transit_ = 0;
    int units_ = 0;
};

struct SourceConfig {
    Distribution interarrival = Distribution::exponential(10.0);
    std::vector<std::pair<int, double>> mix;  // part type -> weight
    std::map<int, int> priority;              // part type -> priority
    bool wait_when_blocked = false;           // false: arrivals are lost when nothing downstream accepts
    long long max_parts = -1;
};
class Source final : public Node {
public:
    Source(Plant& plant, std::string name, SourceConfig cfg);
    [[nodiscard]] std::string_view kind() const override { return "source"; }
    void start() override;
    void on_event(const Event& ev) override;
    void on_space() override;
    [[nodiscard]] int units_inside() const override { return held_ == kNoPart ? 0 : 1; }
    [[nodiscard]] bool can_inject() const override;

private:
    void spawn();
    void schedule_next();
    SourceConfig cfg_;
    RngStream rng_arrival_, rng_mix_, rng_quality_;
    PartId held_ = kNoPart;
    long long spawned_ = 0;
};

// Terminal node. `completed` sinks count as shipped output, others as scrap.
class Sink final : public Node {
public:
    Sink(Plant& plant, std::string name, bool completed) : Node(plant, std::move(name)), completed_(completed) {}
    [[nodiscard]] std::string_view kind() const override { return "sink"; }
    [[nodiscard]] bool can_accept(const Part&, bool) const override { return true; }
    void accept(PartId pid) override;
    [[nodiscard]] bool counts_as_completed() const noexcept { return completed_; }

private:
    bool completed_;
};

// Shared parameter handling, reusable by custom nodes that extend Machine.
[[nodiscard]] MachineConfig read_machine_config(ParamReader& r, Plant& plant);

}  // namespace factory
