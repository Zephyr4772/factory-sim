#include "factory/registry.hpp"

#include <algorithm>

#include "factory/nodes.hpp"

namespace factory {

namespace {

std::string ctx_of(const std::string& name, const char* type) { return "node '" + name + "' (" + type + ")"; }

ParamSpec spec(std::string name, std::string kind, std::string doc, json def = nullptr,
               std::vector<std::string> choices = {}) {
    return {std::move(name), std::move(kind), std::move(doc), std::move(def), std::move(choices)};
}

const json kDefaultProcess = {{"dist", "normal"}, {"mean", 10.0}, {"stddev", 1.5}};

Discipline parse_discipline(ParamReader& r, const std::string& s) {
    if (s == "fifo") return Discipline::Fifo;
    if (s == "lifo") return Discipline::Lifo;
    if (s == "priority") return Discipline::Priority;
    r.fail("discipline must be fifo, lifo or priority");
}

}  // namespace

// ------------------------------------------------------------------ machine params

std::vector<ParamSpec> machine_param_specs() {
    return {
        spec("capacity", "integer", "Buffer slots for waiting parts (the machine holds one more). -1 = unbounded.", 5),
        spec("process", "distribution", "Processing time in seconds.", kDefaultProcess),
        spec("process_by_type", "object", "Per part-type processing time overrides: {\"B\": {...}}.", json::object()),
        spec("setup", "distribution", "Changeover time added when the part type changes.", nullptr),
        spec("discipline", "enum", "Which waiting part is served next.", "fifo", {"fifo", "lifo", "priority"}),
        spec("on_full", "enum", "Full buffer: block upstream (backpressure) or drop the arriving part.", "block",
             {"block", "drop"}),
        spec("mtbf", "number", "Mean time between failures, seconds (0 = never fails).", 0.0),
        spec("mttr", "number", "Mean time to repair, seconds.", 0.0),
        spec("failure_clock", "enum", "MTBF counts only busy time (operating) or calendar time (wall).", "operating",
             {"operating", "wall"}),
        spec("on_failure", "enum", "Part in progress when the machine breaks: continue after repair, redo, or scrap.",
             "resume", {"resume", "restart", "scrap"}),
        spec("process_scale", "number", "Multiplier on every processing time (what-if knob).", 1.0),
        spec("repair_scale", "number", "Multiplier on every repair time (what-if knob).", 1.0),
    };
}

MachineConfig read_machine_config(ParamReader& r, Plant& plant) {
    MachineConfig c;
    c.capacity = r.integer("capacity", 5);
    if (c.capacity < -1) r.fail("capacity must be >= -1");
    c.discipline = parse_discipline(r, r.string("discipline", "fifo"));
    const std::string on_full = r.string("on_full", "block");
    if (on_full != "block" && on_full != "drop") r.fail("on_full must be block or drop");
    c.drop_when_full = on_full == "drop";
    c.process = r.dist("process", parse_distribution(kDefaultProcess, r.context()));
    if (const json* by_type = r.object("process_by_type")) {
        if (!by_type->is_object()) r.fail("process_by_type must be an object");
        for (auto it = by_type->begin(); it != by_type->end(); ++it)
            c.process_by_type[plant.type_id(it.key())] =
                parse_distribution(it.value(), r.context() + ".process_by_type." + it.key());
    }
    c.setup = r.opt_dist("setup");
    c.mtbf_s = r.number("mtbf", 0.0);
    c.mttr_s = r.number("mttr", 0.0);
    if (c.mtbf_s < 0 || c.mttr_s < 0) r.fail("mtbf and mttr must be >= 0");
    if (c.mtbf_s > 0 && c.mttr_s <= 0) r.fail("mttr must be > 0 when mtbf is set");
    const std::string clock = r.string("failure_clock", "operating");
    if (clock != "operating" && clock != "wall") r.fail("failure_clock must be operating or wall");
    c.clock = clock == "wall" ? FailureClock::Wall : FailureClock::Operating;
    const std::string policy = r.string("on_failure", "resume");
    if (policy == "resume") c.on_failure = FailurePolicy::Resume;
    else if (policy == "restart") c.on_failure = FailurePolicy::Restart;
    else if (policy == "scrap") c.on_failure = FailurePolicy::Scrap;
    else r.fail("on_failure must be resume, restart or scrap");
    c.process_scale = r.number("process_scale", 1.0);
    c.repair_scale = r.number("repair_scale", 1.0);
    if (c.process_scale <= 0 || c.repair_scale <= 0) r.fail("process_scale and repair_scale must be > 0");
    return c;
}

// ------------------------------------------------------------------ registry

void NodeRegistry::add(NodeTypeInfo info) {
    auto it = std::find_if(types_.begin(), types_.end(), [&](const auto& t) { return t.name == info.name; });
    if (it != types_.end()) *it = std::move(info);
    else types_.push_back(std::move(info));
}

const NodeTypeInfo* NodeRegistry::find(std::string_view name) const {
    for (const auto& t : types_)
        if (t.name == name) return &t;
    return nullptr;
}

json NodeRegistry::describe() const {
    json out = json::array();
    for (const auto& t : types_) {
        json params = json::array();
        for (const auto& p : t.params) {
            json pj = {{"name", p.name}, {"kind", p.kind}, {"doc", p.doc}, {"default", p.def}};
            if (!p.choices.empty()) pj["choices"] = p.choices;
            params.push_back(std::move(pj));
        }
        out.push_back({{"name", t.name}, {"category", t.category}, {"description", t.description}, {"params", params}});
    }
    return out;
}

NodeRegistry NodeRegistry::with_defaults() {
    NodeRegistry r;
    register_builtin_nodes(r);
    register_custom_nodes(r);
    return r;
}

void register_builtin_nodes(NodeRegistry& reg) {
    // ---- source ----
    reg.add({"source", "source", "Creates parts. Mix of part types, random inter-arrival times.",
             {spec("interarrival", "distribution", "Time between arrivals, seconds.",
                   json{{"dist", "exponential"}, {"mean", 12.0}}),
              spec("mix", "object", "Part types and their relative weights, e.g. {\"A\": 3, \"B\": 1}.",
                   json{{"part", 1}}),
              spec("priority", "object", "Priority per part type (used by 'priority' queue discipline).", json::object()),
              spec("on_blocked", "enum", "When nothing downstream accepts: lose the arrival, or wait.", "drop",
                   {"drop", "wait"}),
              spec("max_parts", "integer", "Stop after this many parts (-1 = never).", -1)},
             [](Plant& plant, const std::string& name, const json& params) -> std::unique_ptr<Node> {
                 ParamReader r(params, ctx_of(name, "source"));
                 SourceConfig c;
                 c.interarrival = r.dist("interarrival", Distribution::exponential(12.0));
                 if (c.interarrival.mean() <= 0) r.fail("interarrival must have a positive mean");
                 const json* mix = r.object("mix");
                 if (mix) {
                     if (!mix->is_object() || mix->empty()) r.fail("mix must be a non-empty object");
                     for (auto it = mix->begin(); it != mix->end(); ++it) {
                         if (!it.value().is_number() || it.value().get<double>() <= 0)
                             r.fail("mix weights must be positive numbers");
                         c.mix.emplace_back(plant.type_id(it.key()), it.value().get<double>());
                     }
                 } else {
                     c.mix.emplace_back(plant.type_id("part"), 1.0);
                 }
                 if (const json* pr = r.object("priority")) {
                     if (!pr->is_object()) r.fail("priority must be an object");
                     for (auto it = pr->begin(); it != pr->end(); ++it) {
                         if (!it.value().is_number_integer()) r.fail("priority values must be integers");
                         c.priority[plant.type_id(it.key())] = it.value().get<int>();
                     }
                 }
                 const std::string ob = r.string("on_blocked", "drop");
                 if (ob != "drop" && ob != "wait") r.fail("on_blocked must be drop or wait");
                 c.wait_when_blocked = ob == "wait";
                 c.max_parts = r.integer("max_parts", -1);
                 r.finish();
                 return std::make_unique<Source>(plant, name, std::move(c));
             }});

    // ---- machine ----
    reg.add({"machine", "station",
             "A processing station: bounded buffer, stochastic service, optional failures and changeovers.",
             machine_param_specs(),
             [](Plant& plant, const std::string& name, const json& params) -> std::unique_ptr<Node> {
                 ParamReader r(params, ctx_of(name, "machine"));
                 MachineConfig c = read_machine_config(r, plant);
                 r.finish();
                 return std::make_unique<Machine>(plant, name, std::move(c));
             }});

    // ---- batcher ----
    {
        auto specs = machine_param_specs();
        specs.push_back(spec("batch_size", "integer", "Parts per batch.", 4));
        specs.push_back(spec("timeout", "number", "Release a partial batch after this many seconds (0 = wait for a full batch).", 0.0));
        specs.push_back(spec("output_type", "string", "Part type of the batch (default: type of the first part).", nullptr));
        reg.add({"batcher", "station", "Groups parts into batches, then processes the batch as one unit.", specs,
                 [](Plant& plant, const std::string& name, const json& params) -> std::unique_ptr<Node> {
                     ParamReader r(params, ctx_of(name, "batcher"));
                     BatcherConfig c;
                     c.machine = read_machine_config(r, plant);
                     c.batch_size = r.integer("batch_size", 4);
                     c.timeout_s = r.number("timeout", 0.0);
                     if (r.has("output_type")) c.output_type = plant.type_id(r.string("output_type", ""));
                     r.finish();
                     if (c.batch_size < 1) r.fail("batch_size must be >= 1");
                     if (c.timeout_s < 0) r.fail("timeout must be >= 0");
                     if (c.machine.capacity >= 0 && c.machine.capacity < c.batch_size)
                         r.fail("capacity must be >= batch_size (or -1), otherwise a full batch can never form");
                     return std::make_unique<Batcher>(plant, name, std::move(c));
                 }});
    }

    // ---- assembler ----
    {
        auto specs = machine_param_specs();
        for (auto& p : specs)
            if (p.name == "capacity") p.doc = "Buffer slots PER PART TYPE (-1 = unbounded). Must cover the kit count.";
        specs.push_back(spec("kit", "object", "Parts required per assembly: {\"body\": 1, \"wheel\": 4}.",
                             json{{"A", 1}, {"B", 1}}));
        specs.push_back(spec("output_type", "string", "Part type of the assembled product.", "assembly"));
        reg.add({"assembler", "station", "Waits for a complete kit of part types, then merges them into one product.", specs,
                 [](Plant& plant, const std::string& name, const json& params) -> std::unique_ptr<Node> {
                     ParamReader r(params, ctx_of(name, "assembler"));
                     AssemblerConfig c;
                     c.machine = read_machine_config(r, plant);
                     const json* kit = r.object("kit");
                     if (!kit || !kit->is_object() || kit->empty()) r.fail("assembler needs a non-empty 'kit' object");
                     int kit_max = 0;
                     for (auto it = kit->begin(); it != kit->end(); ++it) {
                         if (!it.value().is_number_integer() || it.value().get<int>() < 1)
                             r.fail("kit counts must be integers >= 1");
                         c.kit.emplace_back(plant.type_id(it.key()), it.value().get<int>());
                         kit_max = std::max(kit_max, it.value().get<int>());
                     }
                     c.output_type = plant.type_id(r.string("output_type", "assembly"));
                     r.finish();
                     if (c.machine.capacity >= 0 && c.machine.capacity < kit_max)
                         r.fail("capacity is per part type and must be >= the largest kit count (" + std::to_string(kit_max) + ") or -1");
                     return std::make_unique<Assembler>(plant, name, std::move(c));
                 }});
    }

    // ---- delay ----
    reg.add({"delay", "transport", "A conveyor: every part spends a transit time inside; unlimited parallelism.",
             {spec("transit", "distribution", "Transit time, seconds.", json{{"dist", "constant"}, {"value", 5.0}}),
              spec("capacity", "integer", "Max parts on the conveyor (-1 = unbounded).", -1)},
             [](Plant& plant, const std::string& name, const json& params) -> std::unique_ptr<Node> {
                 ParamReader r(params, ctx_of(name, "delay"));
                 const Distribution d = r.dist("transit", Distribution::constant(5.0));
                 const int cap = r.integer("capacity", -1);
                 r.finish();
                 if (cap < -1) r.fail("capacity must be >= -1");
                 return std::make_unique<DelayNode>(plant, name, d, cap);
             }});

    // ---- sink ----
    reg.add({"sink", "sink", "Terminal node. 'completed' sinks count as shipped output, 'scrapped' as waste.",
             {spec("counts_as", "enum", "How parts arriving here are accounted.", "completed", {"completed", "scrapped"})},
             [](Plant& plant, const std::string& name, const json& params) -> std::unique_ptr<Node> {
                 ParamReader r(params, ctx_of(name, "sink"));
                 const std::string as = r.string("counts_as", "completed");
                 r.finish();
                 if (as != "completed" && as != "scrapped") r.fail("counts_as must be completed or scrapped");
                 return std::make_unique<Sink>(plant, name, as == "completed");
             }});
}

}  // namespace factory
