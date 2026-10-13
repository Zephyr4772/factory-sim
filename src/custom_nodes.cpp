// ===========================================================================
//  CUSTOM NODES: this file is the template for writing your own node type.
//
//  Recipe:
//    1. Derive from Node (a blank slate) or Machine (buffer + service + failures).
//    2. Override the hooks you need. Here only route_override() and resolve().
//    3. Describe the parameters (ParamSpec) and write a factory lambda.
//    4. Register it in register_custom_nodes() below.
//  That is all: the config loader, the validator, `factory_sim types` and the
//  Studio's property panel all pick it up automatically.
// ===========================================================================
#include "factory/nodes.hpp"
#include "factory/registry.hpp"

namespace factory {

namespace {

// An inspection station with a quality criterion and a rework budget.
//   quality >= threshold                      -> pass_to
//   quality <  threshold, reworks < max       -> rework_to  (quality is re-rolled: the repair worked, maybe)
//   quality <  threshold, rework budget spent -> scrap_to
class QualityGate final : public Machine {
public:
    struct Cfg {
        double threshold = 0.15;
        int max_reworks = 2;
        std::string pass_to, rework_to, scrap_to;
    };

    QualityGate(Plant& plant, std::string name, MachineConfig mc, Cfg cfg)
        : Machine(plant, std::move(name), std::move(mc)), cfg_(std::move(cfg)), rng_(make_stream("gate")) {}

    [[nodiscard]] std::string_view kind() const override { return "quality_gate"; }

    // Phase 2 of loading: all edges exist, so target names can become edge indices.
    void resolve() override {
        pass_ = find_edge(cfg_.pass_to, "pass_to", true);
        rework_ = find_edge(cfg_.rework_to, "rework_to", false);
        scrap_ = find_edge(cfg_.scrap_to, "scrap_to", false);
    }

protected:
    // Called once per finished part: pick the destination edge ourselves.
    std::optional<int> route_override(const Part& p) override {
        if (p.quality >= cfg_.threshold) return pass_;
        if (rework_ >= 0 && p.reworks < cfg_.max_reworks) {
            Part& mine = plant().part(p.id);  // the gate counts reworks itself (no "rework" flag on the edge)
            ++mine.reworks;
            mine.quality = rng_.uniform();
            return rework_;
        }
        return scrap_ >= 0 ? scrap_ : pass_;
    }

private:
    int find_edge(const std::string& target, const char* param, bool required) const {
        if (target.empty()) {
            if (required) throw ConfigError("node '" + name() + "' (quality_gate): '" + param + "' is required");
            return -1;
        }
        for (std::size_t i = 0; i < edges().size(); ++i)
            if (plant().node(edges()[i].to).name() == target) return static_cast<int>(i);
        throw ConfigError("node '" + name() + "' (quality_gate): " + param + " '" + target +
                          "' is not one of its downstream nodes");
    }

    Cfg cfg_;
    RngStream rng_;
    int pass_ = -1, rework_ = -1, scrap_ = -1;
};

}  // namespace

void register_custom_nodes(NodeRegistry& reg) {
    auto specs = machine_param_specs();
    specs.push_back({"threshold", "number", "Parts with quality below this fail inspection (quality ~ U[0,1]).", 0.15, {}});
    specs.push_back({"max_reworks", "integer", "Rework attempts before a failing part is scrapped.", 2, {}});
    specs.push_back({"pass_to", "string", "Downstream node id for good parts.", nullptr, {}});
    specs.push_back({"rework_to", "string", "Downstream node id for rework (empty = no rework).", nullptr, {}});
    specs.push_back({"scrap_to", "string", "Downstream node id for scrap (empty = pass anyway).", nullptr, {}});

    reg.add({"quality_gate", "station",
             "CUSTOM: inspection with a quality threshold and rework budget (shows how to extend Machine).",
             std::move(specs),
             [](Plant& plant, const std::string& name, const json& params) -> std::unique_ptr<Node> {
                 ParamReader r(params, "node '" + name + "' (quality_gate)");
                 MachineConfig mc = read_machine_config(r, plant);
                 QualityGate::Cfg c;
                 c.threshold = r.number("threshold", 0.15);
                 c.max_reworks = r.integer("max_reworks", 2);
                 c.pass_to = r.string("pass_to", "");
                 c.rework_to = r.string("rework_to", "");
                 c.scrap_to = r.string("scrap_to", "");
                 r.finish();
                 if (c.threshold < 0 || c.threshold > 1) r.fail("threshold must be within [0, 1]");
                 if (c.max_reworks < 0) r.fail("max_reworks must be >= 0");
                 return std::make_unique<QualityGate>(plant, name, std::move(mc), std::move(c));
             }});
}

}  // namespace factory
