#include "factory/nodes.hpp"

#include <algorithm>

namespace factory {

// =============================== Machine ===============================

Machine::Machine(Plant& plant, std::string name, MachineConfig cfg)
    : Node(plant, std::move(name)),
      cfg_(std::move(cfg)),
      rng_proc_(make_stream("process")),
      rng_setup_(make_stream("setup")),
      rng_fail_(make_stream("fail")),
      rng_repair_(make_stream("repair")) {}

void Machine::start() {
    if (cfg_.mtbf_s <= 0) return;
    ttf_remaining_ = to_ticks(rng_fail_.exponential(cfg_.mtbf_s));
    if (cfg_.clock == FailureClock::Wall) {
        ++fail_gen_;
        plant().schedule(ttf_remaining_, EventType::MachineFailed, id(), fail_gen_);
    }
}

bool Machine::full() const {
    return cfg_.capacity >= 0 && static_cast<int>(queue_.size()) >= cfg_.capacity;
}

bool Machine::can_accept(const Part&, bool bypass) const {
    return cfg_.drop_when_full || bypass || !full();
}

void Machine::accept(PartId pid) {
    if (full() && cfg_.drop_when_full) {
        ++stats_.lost_units;
        plant().drop(pid, *this, false);
        return;
    }
    queue_.push_back({pid, now()});
    queued_units_ += plant().part(pid).units;
    note_queue();
    after_enqueue();
    try_start();
}

std::size_t Machine::select_index() const {
    switch (cfg_.discipline) {
        case Discipline::Fifo: return 0;
        case Discipline::Lifo: return queue_.size() - 1;
        case Discipline::Priority: {
            std::size_t best = 0;
            for (std::size_t i = 1; i < queue_.size(); ++i)
                if (plant().part(queue_[i].id).priority > plant().part(queue_[best].id).priority) best = i;
            return best;
        }
    }
    return 0;
}

PartId Machine::take_at(std::size_t index) {
    const Queued q = queue_[index];
    queue_.erase(queue_.begin() + static_cast<std::ptrdiff_t>(index));
    queued_units_ -= plant().part(q.id).units;
    stats_.wait_sum += now() - q.enter;
    ++stats_.wait_count;
    note_queue();
    plant().signal_space(id());  // a slot opened: let blocked upstreams retry
    return q.id;
}

bool Machine::pick_work(std::vector<PartId>& out) {
    if (queue_.empty()) return false;
    out.push_back(take_at(select_index()));
    return true;
}

double Machine::service_seconds(const std::vector<PartId>& inputs) {
    const int type = plant().part(inputs.front()).type;
    auto it = cfg_.process_by_type.find(type);
    const Distribution& d = it != cfg_.process_by_type.end() ? it->second : cfg_.process;
    double s = d.sample(rng_proc_) * cfg_.process_scale;
    if (cfg_.setup && last_type_ >= 0 && last_type_ != type) s += cfg_.setup->sample(rng_setup_);
    last_type_ = type;
    return s;
}

void Machine::try_start() {
    if (state() != NodeState::Idle) return;
    inputs_.clear();
    if (!pick_work(inputs_)) return;
    job_units_ = 0;
    for (PartId pid : inputs_) job_units_ += plant().part(pid).units;
    job_duration_ = to_ticks(service_seconds(inputs_));
    begin_busy(job_duration_);
}

void Machine::begin_busy(Tick duration) {
    remaining_ = duration;
    end_at_ = now() + duration;
    busy_since_ = now();
    ++gen_;
    set_state(NodeState::Busy);
    plant().schedule(duration, EventType::ProcessingCompleted, id(), gen_);
    if (cfg_.mtbf_s > 0 && cfg_.clock == FailureClock::Operating) {
        ++fail_gen_;
        plant().schedule(std::max<Tick>(ttf_remaining_, 0), EventType::MachineFailed, id(), fail_gen_);
    }
}

void Machine::leave_busy() {
    if (cfg_.mtbf_s > 0 && cfg_.clock == FailureClock::Operating) {
        ttf_remaining_ -= now() - busy_since_;  // only operating time wears the machine out
        ++fail_gen_;                            // cancels the pending failure
    }
}

void Machine::push_out() {
    if (try_forward(job_part_)) {
        job_part_ = kNoPart;
        job_units_ = 0;
        set_state(NodeState::Idle);
        try_start();
    } else {
        set_state(NodeState::Blocked);
    }
}

void Machine::on_space() {
    if (state() == NodeState::Blocked && job_part_ != kNoPart) push_out();
    // A Down machine retries when it is repaired.
}

void Machine::on_event(const Event& ev) {
    switch (ev.type) {
        case EventType::ProcessingCompleted: on_complete(ev.gen); break;
        case EventType::MachineFailed: on_failure(ev.gen); break;
        case EventType::MachineRepaired: on_repaired(); break;
        case EventType::Timer: on_timer(ev.gen); break;
        default: break;
    }
}

void Machine::on_complete(std::uint32_t gen) {
    if (gen != gen_ || state() != NodeState::Busy) return;  // stale: the machine broke down mid-cycle
    leave_busy();
    stats_.processed_units += static_cast<std::uint64_t>(job_units_);
    job_part_ = make_output(inputs_);
    inputs_.clear();
    job_units_ = plant().part(job_part_).units;
    on_service_done(job_part_);
    push_out();
}

void Machine::on_failure(std::uint32_t gen) {
    if (gen != fail_gen_ || state() == NodeState::Down) return;  // stale
    ++stats_.failures;
    if (state() == NodeState::Busy) {
        leave_busy();
        remaining_ = end_at_ - now();
        ++gen_;  // invalidates the pending ProcessingCompleted
        switch (cfg_.on_failure) {
            case FailurePolicy::Resume: break;
            case FailurePolicy::Restart: remaining_ = job_duration_; break;
            case FailurePolicy::Scrap:
                for (PartId pid : inputs_) plant().retire(pid, *this, false);
                inputs_.clear();
                job_units_ = 0;
                remaining_ = 0;
                break;
        }
    }
    set_state(NodeState::Down);
    const double repair = rng_repair_.exponential(cfg_.mttr_s) * cfg_.repair_scale;
    plant().schedule(to_ticks(repair), EventType::MachineRepaired, id());
}

void Machine::on_repaired() {
    if (state() != NodeState::Down) return;
    if (cfg_.mtbf_s > 0) {
        const Tick ttf = to_ticks(rng_fail_.exponential(cfg_.mtbf_s));
        if (cfg_.clock == FailureClock::Wall) {
            ++fail_gen_;
            plant().schedule(ttf, EventType::MachineFailed, id(), fail_gen_);
        } else {
            ttf_remaining_ = ttf;
        }
    }
    if (job_part_ != kNoPart) {          // finished part was waiting to leave
        set_state(NodeState::Idle);
        push_out();
    } else if (!inputs_.empty()) {       // resume the interrupted job
        begin_busy(remaining_);
    } else {
        set_state(NodeState::Idle);
        try_start();
    }
}

json Machine::describe() const {
    return {{"capacity", cfg_.capacity},
            {"mtbf", cfg_.mtbf_s},
            {"mttr", cfg_.mttr_s},
            {"process_mean", cfg_.process.mean() * cfg_.process_scale},
            {"process", cfg_.process.describe()}};
}

// =============================== Batcher ===============================

Batcher::Batcher(Plant& plant, std::string name, BatcherConfig cfg)
    : Machine(plant, std::move(name), cfg.machine), bcfg_(std::move(cfg)) {}

void Batcher::arm_timer() {
    if (bcfg_.timeout_s <= 0 || queue_.empty()) return;
    const Tick due = queue_.front().enter + to_ticks(bcfg_.timeout_s);
    plant().schedule(std::max<Tick>(due - now(), 0), EventType::Timer, id(), ++timer_gen_);
}

void Batcher::after_enqueue() {
    if (queue_.size() == 1) arm_timer();
}

void Batcher::on_timer(std::uint32_t gen) {
    if (gen == timer_gen_) try_start();
}

bool Batcher::pick_work(std::vector<PartId>& out) {
    const auto n = static_cast<std::size_t>(bcfg_.batch_size);
    const bool complete = queue_.size() >= n;
    const bool timed_out = bcfg_.timeout_s > 0 && !queue_.empty() &&
                           now() - queue_.front().enter >= to_ticks(bcfg_.timeout_s);
    if (!complete && !timed_out) return false;
    const std::size_t take = std::min(n, queue_.size());
    for (std::size_t i = 0; i < take; ++i) out.push_back(take_at(0));
    arm_timer();  // leftovers get their own deadline
    return true;
}

PartId Batcher::make_output(std::vector<PartId>& inputs) {
    const int type = bcfg_.output_type ? *bcfg_.output_type : plant().part(inputs.front()).type;
    return plant().merge(inputs, type);
}

// =============================== Assembler ===============================

Assembler::Assembler(Plant& plant, std::string name, AssemblerConfig cfg)
    : Machine(plant, std::move(name), cfg.machine), acfg_(std::move(cfg)) {}

bool Assembler::can_accept(const Part& p, bool bypass) const {
    const int cap = config().capacity;
    if (bypass || cap < 0 || config().drop_when_full) return true;
    int same_type = 0;
    for (const Queued& q : queue_)
        if (plant().part(q.id).type == p.type) ++same_type;
    return same_type < cap;
}

bool Assembler::pick_work(std::vector<PartId>& out) {
    std::vector<std::size_t> picked;
    for (const auto& [type, count] : acfg_.kit) {
        int need = count;
        for (std::size_t i = 0; i < queue_.size() && need > 0; ++i)
            if (plant().part(queue_[i].id).type == type) {
                picked.push_back(i);
                --need;
            }
        if (need > 0) return false;  // kit incomplete: keep waiting
    }
    std::sort(picked.rbegin(), picked.rend());  // erase from the back so indices stay valid
    for (std::size_t i : picked) out.push_back(take_at(i));
    return true;
}

PartId Assembler::make_output(std::vector<PartId>& inputs) {
    const int type = acfg_.output_type ? *acfg_.output_type : plant().part(inputs.front()).type;
    return plant().merge(inputs, type);
}

// =============================== DelayNode ===============================

DelayNode::DelayNode(Plant& plant, std::string name, Distribution transit, int capacity)
    : Node(plant, std::move(name)), transit_(transit), capacity_(capacity), rng_(make_stream("transit")) {}

bool DelayNode::can_accept(const Part&, bool bypass) const {
    return bypass || capacity_ < 0 || queue_length() < capacity_;
}

void DelayNode::accept(PartId pid) {
    ++in_transit_;
    units_ += plant().part(pid).units;
    plant().schedule(to_ticks(transit_.sample(rng_)), EventType::TransitCompleted, id(), 0, pid);
    if (state() == NodeState::Idle) set_state(NodeState::Busy);
    note_queue();
}

void DelayNode::on_event(const Event& ev) {
    if (ev.type != EventType::TransitCompleted) return;
    --in_transit_;
    ready_.push_back(ev.part);
    pump();
}

void DelayNode::pump() {
    while (!ready_.empty()) {
        const PartId pid = ready_.front();
        const int units = plant().part(pid).units;
        if (!try_forward(pid)) {
            set_state(NodeState::Blocked);
            return;
        }
        ready_.pop_front();
        units_ -= units;
        stats_.processed_units += static_cast<std::uint64_t>(units);
        note_queue();
        plant().signal_space(id());
    }
    set_state(in_transit_ > 0 ? NodeState::Busy : NodeState::Idle);
}

// =============================== Source ===============================

Source::Source(Plant& plant, std::string name, SourceConfig cfg)
    : Node(plant, std::move(name)),
      cfg_(std::move(cfg)),
      rng_arrival_(make_stream("arrival")),
      rng_mix_(make_stream("mix")),
      rng_quality_(make_stream("quality")) {}

void Source::schedule_next() {
    if (cfg_.max_parts >= 0 && spawned_ >= cfg_.max_parts) return;
    plant().schedule(to_ticks(cfg_.interarrival.sample(rng_arrival_)), EventType::PartSpawned, id());
}

void Source::start() { schedule_next(); }

void Source::on_event(const Event& ev) {
    if (ev.type == EventType::PartSpawned) spawn();
}

void Source::spawn() {
    std::vector<double> weights;
    weights.reserve(cfg_.mix.size());
    for (const auto& m : cfg_.mix) weights.push_back(m.second);
    const int type = cfg_.mix[rng_mix_.pick(weights)].first;
    auto pr = cfg_.priority.find(type);
    const PartId pid = plant().create_part(type, rng_quality_.uniform(), pr == cfg_.priority.end() ? 0 : pr->second);
    ++spawned_;
    if (try_forward(pid)) {
        schedule_next();
    } else if (cfg_.wait_when_blocked) {
        held_ = pid;  // stop arriving until the line has room
        set_state(NodeState::Blocked);
    } else {
        abandon_forward();
        ++stats_.lost_units;
        plant().drop(pid, *this, true);
        schedule_next();
    }
}

bool Source::can_inject() const {
    if (state() == NodeState::Blocked) return false;  // paused until the line frees up
    if (cfg_.max_parts >= 0 && spawned_ >= cfg_.max_parts) return false;
    for (const auto& [type, weight] : cfg_.mix) {
        Part probe;
        probe.type = type;
        for (const Edge& e : edges())
            if (plant().node(e.to).can_accept(probe, e.bypass)) return true;
    }
    return false;
}

void Source::on_space() {
    if (held_ == kNoPart) return;
    if (try_forward(held_)) {
        held_ = kNoPart;
        set_state(NodeState::Idle);
        schedule_next();
    }
}

// =============================== Sink ===============================

void Sink::accept(PartId pid) {
    stats_.processed_units += static_cast<std::uint64_t>(plant().part(pid).units);
    plant().retire(pid, *this, completed_);
}

}  // namespace factory
