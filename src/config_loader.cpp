#include "factory/config_loader.hpp"

#include <algorithm>
#include <functional>
#include <set>

namespace factory {

namespace {

const std::set<std::string> kReservedNodeKeys = {"id", "type", "label", "name", "x", "y", "params",
                                                "routing", "downstream", "ui", "color"};

// ------------------------------------------------------------------ legacy format

json distribution_from_legacy(const std::string& kind, double mean, double stddev) {
    if (kind == "constant") return {{"dist", "constant"}, {"value", mean}};
    if (kind == "normal") return {{"dist", "normal"}, {"mean", mean}, {"stddev", stddev}};
    return {{"dist", "exponential"}, {"mean", mean}};
}

}  // namespace

json normalize_config(const json& config) {
    if (!config.is_object()) throw ConfigError("config must be a JSON object");
    if (!config.contains("stations")) return config;

    json out = json::object();
    if (config.contains("simulation")) out["simulation"] = config["simulation"];
    json nodes = json::array();
    bool needs_exit = false;

    if (config.contains("ingestion")) {
        const json& ing = config["ingestion"];
        if (!ing.contains("target_station")) throw ConfigError("ingestion.target_station is required");
        nodes.push_back({{"id", "ingestion"},
                         {"type", "source"},
                         {"label", "Ingestion"},
                         {"interarrival", distribution_from_legacy(ing.value("distribution", "exponential"),
                                                                   ing.value("arrival_interval_mean", 10.0), 0.0)},
                         {"downstream", json::array({ing["target_station"]})}});
    }
    for (const json& st : config["stations"]) {
        json node = {{"id", st.value("id", "")}, {"type", "machine"}, {"label", st.value("name", st.value("id", ""))}};
        node["capacity"] = st.value("buffer_capacity", 5);
        node["process"] = distribution_from_legacy(st.value("distribution", "constant"),
                                                   st.value("process_time_mean", 1.0),
                                                   st.value("process_time_stddev", 0.0));
        if (st.value("mtbf", 0.0) > 0) {
            node["mtbf"] = st["mtbf"];
            node["mttr"] = st.value("mttr", 60.0);
        }
        json down = json::array();
        double total = 0;
        if (st.contains("downstream"))
            for (const json& d : st["downstream"]) {
                const double p = d.value("probability", 1.0);
                total += p;
                down.push_back({{"to", d.value("station_id", "")}, {"weight", p}});
            }
        if (total < 1.0 - 1e-9) {
            down.push_back({{"to", "__shipped"}, {"weight", 1.0 - total}});
            needs_exit = true;
        }
        node["downstream"] = down;
        nodes.push_back(std::move(node));
    }
    if (needs_exit) nodes.push_back({{"id", "__shipped"}, {"type", "sink"}, {"label", "Shipped"}});
    out["nodes"] = std::move(nodes);
    return out;
}

namespace {

// ------------------------------------------------------------------ node type templates

struct ResolvedType {
    const NodeTypeInfo* info = nullptr;
    json params = json::object();
    json routing;  // null = none
};

ResolvedType resolve_type(const std::string& type, const json& templates, const NodeRegistry& reg,
                          const std::string& ctx) {
    std::vector<const json*> chain;
    std::string cur = type;
    for (int depth = 0;; ++depth) {
        if (depth > 16) throw ConfigError(ctx + ": node_types inheritance is too deep or circular");
        if (const NodeTypeInfo* info = reg.find(cur)) {
            ResolvedType out;
            out.info = info;
            for (auto it = chain.rbegin(); it != chain.rend(); ++it) {  // base-most template first
                if ((*it)->contains("params")) {
                    for (auto p = (**it)["params"].begin(); p != (**it)["params"].end(); ++p) out.params[p.key()] = p.value();
                }
                if ((*it)->contains("routing")) out.routing = (**it)["routing"];
            }
            return out;
        }
        auto it = templates.find(cur);
        if (it == templates.end()) {
            std::string known;
            for (const auto& t : reg.types()) known += (known.empty() ? "" : ", ") + t.name;
            for (auto t = templates.begin(); t != templates.end(); ++t) known += ", " + t.key();
            throw ConfigError(ctx + ": unknown node type '" + cur + "' (known: " + known + ")");
        }
        if (!it->is_object() || !it->contains("base") || !(*it)["base"].is_string())
            throw ConfigError("node_types." + cur + " needs a string 'base'");
        chain.push_back(&*it);
        cur = (*it)["base"].get<std::string>();
    }
}

// ------------------------------------------------------------------ settings

SimSettings parse_settings(const json& cfg, const BuildOptions& opt) {
    SimSettings s;
    if (cfg.contains("simulation")) {
        ParamReader r(cfg["simulation"], "simulation");
        s.duration_s = r.number("duration_seconds", s.duration_s);
        s.seed = static_cast<std::uint64_t>(r.number("random_seed", static_cast<double>(s.seed)));
        s.warmup_s = r.number("warmup_seconds", 0.0);
        s.sample_interval_s = r.number("sample_interval", s.sample_interval_s);
        s.stop_on_deadlock = r.boolean("stop_on_deadlock", true);
        r.finish();
    }
    if (opt.seed) s.seed = *opt.seed;
    if (opt.duration_s) s.duration_s = *opt.duration_s;
    s.trace = opt.trace;
    if (s.duration_s <= 0) throw ConfigError("simulation.duration_seconds must be > 0");
    if (s.warmup_s < 0 || s.warmup_s >= s.duration_s) throw ConfigError("simulation.warmup_seconds must be within [0, duration)");
    if (s.sample_interval_s < 0) throw ConfigError("simulation.sample_interval must be >= 0");
    return s;
}

// ------------------------------------------------------------------ routing

RoutePolicy parse_policy(const std::string& s, const std::string& ctx) {
    if (s == "probabilistic") return RoutePolicy::Probabilistic;
    if (s == "round_robin") return RoutePolicy::RoundRobin;
    if (s == "shortest_queue") return RoutePolicy::ShortestQueue;
    if (s == "first_available") return RoutePolicy::FirstAvailable;
    throw ConfigError(ctx + ": routing.policy must be probabilistic, round_robin, shortest_queue or first_available");
}

int edge_index_for(const Node& n, const std::string& target, const Plant& plant, const std::string& ctx) {
    for (std::size_t i = 0; i < n.edges().size(); ++i)
        if (plant.node(n.edges()[i].to).name() == target) return static_cast<int>(i);
    throw ConfigError(ctx + ": '" + target + "' is not in this node's downstream list");
}

void apply_routing(Node& node, const json& routing, Plant& plant) {
    const std::string ctx = "node '" + node.name() + "' routing";
    ParamReader r(routing, ctx);
    node.routing().policy = parse_policy(r.string("policy", "probabilistic"), ctx);
    if (const json* rules = r.object("rules")) {
        if (!rules->is_array()) r.fail("rules must be an array");
        int ri = 0;
        for (const json& rj : *rules) {
            const std::string rctx = ctx + ".rules[" + std::to_string(ri++) + "]";
            ParamReader rr(rj, rctx);
            RouteRule rule;
            if (const json* m = rr.object("match")) {
                ParamReader mr(*m, rctx + ".match");
                if (mr.has("type")) rule.match.type = plant.type_id(mr.string("type", ""));
                if (mr.has("quality_lt")) rule.match.quality_lt = mr.number("quality_lt", 0);
                if (mr.has("quality_ge")) rule.match.quality_ge = mr.number("quality_ge", 0);
                if (mr.has("reworks_ge")) rule.match.reworks_ge = mr.integer("reworks_ge", 0);
                if (mr.has("reworks_lt")) rule.match.reworks_lt = mr.integer("reworks_lt", 0);
                if (mr.has("priority_ge")) rule.match.priority_ge = mr.integer("priority_ge", 0);
                mr.finish();
            }
            const json* to = rr.object("to");
            if (!to) rr.fail("each rule needs 'to'");
            if (to->is_string()) rule.edges.push_back(edge_index_for(node, to->get<std::string>(), plant, rctx));
            else if (to->is_array())
                for (const json& t : *to) {
                    if (!t.is_string()) rr.fail("'to' must be a string or an array of strings");
                    rule.edges.push_back(edge_index_for(node, t.get<std::string>(), plant, rctx));
                }
            else rr.fail("'to' must be a string or an array of strings");
            rr.finish();
            node.routing().rules.push_back(std::move(rule));
        }
    }
    r.finish();
}

// ------------------------------------------------------------------ structural checks

void check_structure(Plant& plant) {
    const std::size_t n = plant.node_count();
    bool any_source = false, any_sink = false;
    std::vector<std::vector<NodeId>> fwd(n), rev(n);
    for (std::size_t i = 0; i < n; ++i) {
        const Node& node = plant.node(static_cast<NodeId>(i));
        any_source |= node.kind() == "source";
        any_sink |= node.kind() == "sink";
        if (node.kind() == "sink" && !node.edges().empty())
            throw ConfigError("sink '" + node.name() + "' cannot have downstream nodes");
        if (node.kind() != "sink" && node.edges().empty())
            throw ConfigError("node '" + node.name() + "' has no downstream: parts would vanish. Connect it to a sink");
        for (const Edge& e : node.edges()) {
            if (plant.node(e.to).kind() == "source")
                throw ConfigError("node '" + node.name() + "' feeds source '" + plant.node(e.to).name() + "'; sources accept no input");
            fwd[i].push_back(e.to);
            rev[static_cast<std::size_t>(e.to)].push_back(static_cast<NodeId>(i));
        }
    }
    if (!any_source) throw ConfigError("the pipeline needs at least one source node");
    if (!any_sink) throw ConfigError("the pipeline needs at least one sink node");

    auto reach = [&](const std::vector<std::vector<NodeId>>& g, auto&& is_start) {
        std::vector<char> seen(n, 0);
        std::vector<NodeId> stack;
        for (std::size_t i = 0; i < n; ++i)
            if (is_start(plant.node(static_cast<NodeId>(i)))) { seen[i] = 1; stack.push_back(static_cast<NodeId>(i)); }
        while (!stack.empty()) {
            const NodeId cur = stack.back();
            stack.pop_back();
            for (NodeId nx : g[static_cast<std::size_t>(cur)])
                if (!seen[static_cast<std::size_t>(nx)]) { seen[static_cast<std::size_t>(nx)] = 1; stack.push_back(nx); }
        }
        return seen;
    };
    const auto from_src = reach(fwd, [](const Node& x) { return x.kind() == "source"; });
    const auto to_sink = reach(rev, [](const Node& x) { return x.kind() == "sink"; });
    for (std::size_t i = 0; i < n; ++i) {
        const std::string& nm = plant.node(static_cast<NodeId>(i)).name();
        if (!from_src[i]) throw ConfigError("node '" + nm + "' is unreachable from every source");
        if (!to_sink[i]) throw ConfigError("node '" + nm + "' can never reach a sink (dead end)");
    }

    // Warn about cycles in which every buffer is bounded and no edge bypasses capacity:
    // those can deadlock (A waits for B waits for A).
    std::vector<int> index(n, -1), low(n, 0), comp(n, -1);
    std::vector<char> on_stack(n, 0);
    std::vector<NodeId> stack;
    int counter = 0, ncomp = 0;
    std::function<void(NodeId)> strong = [&](NodeId v) {
        const auto vi = static_cast<std::size_t>(v);
        index[vi] = low[vi] = counter++;
        stack.push_back(v);
        on_stack[vi] = 1;
        for (NodeId w : fwd[vi]) {
            const auto wi = static_cast<std::size_t>(w);
            if (index[wi] < 0) { strong(w); low[vi] = std::min(low[vi], low[wi]); }
            else if (on_stack[wi]) low[vi] = std::min(low[vi], index[wi]);
        }
        if (low[vi] == index[vi]) {
            for (;;) {
                const NodeId w = stack.back();
                stack.pop_back();
                on_stack[static_cast<std::size_t>(w)] = 0;
                comp[static_cast<std::size_t>(w)] = ncomp;
                if (w == v) break;
            }
            ++ncomp;
        }
    };
    for (std::size_t i = 0; i < n; ++i)
        if (index[i] < 0) strong(static_cast<NodeId>(i));
    for (int c = 0; c < ncomp; ++c) {
        std::vector<NodeId> members;
        for (std::size_t i = 0; i < n; ++i)
            if (comp[i] == c) members.push_back(static_cast<NodeId>(i));
        bool cyclic = members.size() > 1;
        if (!cyclic)
            for (const Edge& e : plant.node(members[0]).edges()) cyclic |= e.to == members[0];
        if (!cyclic) continue;
        bool slack = false;
        for (NodeId m : members) {
            const Node& node = plant.node(m);
            slack |= node.capacity() < 0;
            for (const Edge& e : node.edges())
                if (comp[static_cast<std::size_t>(e.to)] == c) slack |= e.bypass;
        }
        if (!slack) {
            std::string names;
            for (NodeId m : members) names += (names.empty() ? "" : ", ") + plant.node(m).name();
            plant.warnings().push_back("possible deadlock: the loop {" + names +
                                       "} has only bounded buffers. Set \"bypass\": true on a rework edge or use capacity -1 on one node.");
        }
    }
}

}  // namespace

// ------------------------------------------------------------------ main entry

std::unique_ptr<Plant> build_plant(const json& raw, const NodeRegistry& registry, const BuildOptions& options) {
    const json cfg = normalize_config(raw);
    for (auto it = cfg.begin(); it != cfg.end(); ++it)
        if (it.key() != "simulation" && it.key() != "nodes" && it.key() != "node_types" && it.key() != "name" &&
            it.key() != "description" && it.key() != "ui")
            throw ConfigError("unknown top-level key '" + it.key() + "' (simulation, node_types, nodes)");
    if (!cfg.contains("nodes") || !cfg["nodes"].is_array() || cfg["nodes"].empty())
        throw ConfigError("config needs a non-empty 'nodes' array");

    auto plant = std::make_unique<Plant>(parse_settings(cfg, options));
    const json templates = cfg.contains("node_types") ? cfg["node_types"] : json::object();
    if (!templates.is_object()) throw ConfigError("node_types must be an object");
    for (auto t = templates.begin(); t != templates.end(); ++t)
        if (registry.find(t.key())) throw ConfigError("node_types." + t.key() + " clashes with a built-in node type");

    // Pass 1: create nodes.
    std::vector<json> routings;
    for (const json& nj : cfg["nodes"]) {
        if (!nj.is_object()) throw ConfigError("every entry of 'nodes' must be an object");
        if (!nj.contains("id") || !nj["id"].is_string() || nj["id"].get<std::string>().empty())
            throw ConfigError("every node needs a non-empty string 'id'");
        const std::string id = nj["id"].get<std::string>();
        if (!nj.contains("type") || !nj["type"].is_string()) throw ConfigError("node '" + id + "': missing 'type'");
        const ResolvedType rt = resolve_type(nj["type"].get<std::string>(), templates, registry, "node '" + id + "'");
        json params = rt.params;
        if (nj.contains("params")) {
            if (!nj["params"].is_object()) throw ConfigError("node '" + id + "': 'params' must be an object");
            for (auto p = nj["params"].begin(); p != nj["params"].end(); ++p) params[p.key()] = p.value();
        }
        for (auto p = nj.begin(); p != nj.end(); ++p)
            if (!kReservedNodeKeys.count(p.key())) params[p.key()] = p.value();
        try {
            plant->add_node(rt.info->factory(*plant, id, params));
        } catch (const std::invalid_argument& e) {
            throw ConfigError(e.what());
        }
        routings.push_back(nj.contains("routing") ? nj["routing"] : rt.routing);
    }

    // Pass 2: edges.
    for (std::size_t i = 0; i < cfg["nodes"].size(); ++i) {
        const json& nj = cfg["nodes"][i];
        const std::string id = nj["id"].get<std::string>();
        const NodeId from = *plant->find(id);
        if (!nj.contains("downstream")) continue;
        if (!nj["downstream"].is_array()) throw ConfigError("node '" + id + "': downstream must be an array");
        int di = 0;
        for (const json& dj : nj["downstream"]) {
            const std::string ctx = "node '" + id + "' downstream[" + std::to_string(di++) + "]";
            Edge e;
            std::string target;
            if (dj.is_string()) {
                target = dj.get<std::string>();
            } else {
                ParamReader r(dj, ctx);
                target = r.string("to", "");
                e.weight = r.number("weight", 1.0);
                e.rework = r.boolean("rework", false);
                e.bypass = r.boolean("bypass", false);
                e.label = r.string("label", "");
                r.finish();
            }
            auto to = plant->find(target);
            if (!to) throw ConfigError(ctx + ": unknown target node '" + target + "'");
            if (e.weight <= 0) throw ConfigError(ctx + ": weight must be > 0");
            e.to = *to;
            plant->connect(from, e);
        }
    }

    // Pass 3: routing rules (need the edge lists), structure, resolve names, start.
    for (std::size_t i = 0; i < routings.size(); ++i)
        if (!routings[i].is_null()) {
            const NodeId id = static_cast<NodeId>(i);
            apply_routing(plant->node(id), routings[i], *plant);
        }
    check_structure(*plant);
    plant->finalize();
    return plant;
}

}  // namespace factory
