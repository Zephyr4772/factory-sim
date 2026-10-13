#define _CRT_SECURE_NO_WARNINGS  // std::localtime is fine here: called once, single-threaded
#include "factory/audit.hpp"

#include <algorithm>
#include <cmath>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>

namespace factory {

const char* to_string(Severity s) noexcept {
    switch (s) {
        case Severity::Critical: return "Critical";
        case Severity::High: return "High";
        case Severity::Medium: return "Medium";
        case Severity::Low: return "Low";
        case Severity::Info: return "Info";
    }
    return "?";
}

namespace {

std::string f(double v, int prec = 1) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

const std::set<std::string> kReserved = {"id", "type", "label", "name", "x", "y", "params", "routing", "downstream", "ui", "color"};

struct NodeDef {
    std::string id, label, type, base;
    json params = json::object();
};

// Resolves node_types templates exactly like the loader, so the audit sees effective parameters.
std::vector<NodeDef> node_defs(const json& norm, const NodeRegistry& reg) {
    std::vector<NodeDef> out;
    const json templates = norm.contains("node_types") ? norm["node_types"] : json::object();
    for (const json& nj : norm["nodes"]) {
        NodeDef d;
        d.id = nj.value("id", "");
        d.label = nj.value("label", nj.value("name", d.id));
        d.type = nj.value("type", "");
        std::vector<const json*> chain;
        std::string cur = d.type;
        for (int depth = 0; depth < 16 && !reg.find(cur); ++depth) {
            auto it = templates.find(cur);
            if (it == templates.end()) break;
            chain.push_back(&*it);
            cur = it->value("base", "");
        }
        d.base = cur;
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            if ((*it)->contains("params"))
                for (auto p = (**it)["params"].begin(); p != (**it)["params"].end(); ++p) d.params[p.key()] = p.value();
        if (nj.contains("params"))
            for (auto p = nj["params"].begin(); p != nj["params"].end(); ++p) d.params[p.key()] = p.value();
        for (auto p = nj.begin(); p != nj.end(); ++p)
            if (!kReserved.count(p.key())) d.params[p.key()] = p.value();
        out.push_back(std::move(d));
    }
    return out;
}

std::string compact(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_number_float()) return f(v.get<double>(), 2);
    return v.dump();
}

std::string params_text(const json& params) {
    std::string s;
    for (auto it = params.begin(); it != params.end(); ++it) {
        if (!s.empty()) s += ", ";
        std::string value = compact(it.value());
        if (it.value().is_object() && it.value().contains("dist")) {
            const json& d = it.value();
            value = d["dist"].get<std::string>() + "(";
            bool first = true;
            for (auto k = d.begin(); k != d.end(); ++k) {
                if (k.key() == "dist") continue;
                value += (first ? "" : ",") + compact(k.value());
                first = false;
            }
            value += ")";
        }
        s += it.key() + "=" + value;
    }
    return s.empty() ? "(defaults)" : s;
}

// True if removing `skip` leaves no way to turn raw input into finished output. An assembler needs
// every one of its inputs (all kit parts), any other node needs just one working predecessor.
bool is_single_point(const Plant& plant, NodeId skip) {
    const std::size_t n = plant.node_count();
    std::vector<std::vector<NodeId>> preds(n);
    for (std::size_t i = 0; i < n; ++i)
        for (const Edge& e : plant.node(static_cast<NodeId>(i)).edges()) preds[static_cast<std::size_t>(e.to)].push_back(static_cast<NodeId>(i));
    std::vector<char> live(n, 0);
    for (bool changed = true; changed;) {
        changed = false;
        for (std::size_t i = 0; i < n; ++i) {
            if (live[i] || static_cast<NodeId>(i) == skip) continue;
            const Node& node = plant.node(static_cast<NodeId>(i));
            bool ok;
            if (node.kind() == "source") {
                ok = true;
            } else if (preds[i].empty()) {
                ok = false;
            } else if (node.kind() == "assembler") {
                ok = std::all_of(preds[i].begin(), preds[i].end(), [&](NodeId p) { return live[static_cast<std::size_t>(p)] != 0; });
            } else {
                ok = std::any_of(preds[i].begin(), preds[i].end(), [&](NodeId p) { return live[static_cast<std::size_t>(p)] != 0; });
            }
            if (ok) {
                live[i] = 1;
                changed = true;
            }
        }
    }
    for (std::size_t i = 0; i < n; ++i)
        if (live[i] && plant.node(static_cast<NodeId>(i)).kind() == "sink") return false;
    return true;
}

std::string now_string() {
    const std::time_t t = std::time(nullptr);
    const std::tm* tm = std::localtime(&t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", tm);
    return buf;
}

}  // namespace

AuditReport audit(const json& config, const NodeRegistry& registry, const AuditOptions& opt) {
    AuditReport r;
    const json norm = normalize_config(config);
    r.title = norm.value("name", "Untitled pipeline");
    r.description = norm.value("description", "");
    r.generated = now_string();

    // ---- 1. the reference run (also gives us the plant graph) ----
    auto plant = build_plant(config, registry);
    plant->run();
    r.summary = summarize(*plant);
    r.duration_s = plant->settings().duration_s;
    r.warmup_s = plant->settings().warmup_s;
    r.seed = plant->settings().seed;
    const Summary& s = r.summary;

    // ---- 2. robustness across seeds, 3. what-if levers ----
    if (opt.replications >= 2) {
        r.replication = replicate(config, registry, opt.replications);
        r.has_replication = true;
        r.replications = opt.replications;
    }
    if (opt.include_whatif) {
        r.whatif = what_if(config, registry, std::max(1, opt.whatif_replications));
        r.has_whatif = true;
    }

    // ---- 4. inventory ----
    const std::vector<NodeDef> defs = node_defs(norm, registry);
    for (const NodeDef& d : defs) {
        InventoryRow row;
        row.id = d.id;
        row.label = d.label;
        row.type = d.type;
        row.base_type = d.base;
        row.params = params_text(d.params);
        if (d.params.contains("mtbf") && d.params["mtbf"].is_number()) row.mtbf_s = d.params["mtbf"].get<double>();
        if (d.params.contains("mttr") && d.params["mttr"].is_number()) row.mttr_s = d.params["mttr"].get<double>();
        if (row.mtbf_s > 0 && row.mttr_s > 0) row.availability = row.mtbf_s / (row.mtbf_s + row.mttr_s);
        r.inventory.push_back(std::move(row));
    }
    // ---- findings ----
    auto add = [&](Severity sev, std::string area, std::string subject, std::string issue, std::string rec, int penalty) {
        r.findings.push_back({sev, std::move(area), std::move(subject), std::move(issue), std::move(rec), penalty});
    };
    const double lever_pct = r.has_whatif && !r.whatif.results.empty() ? r.whatif.results.front().delta_pct : 0;
    const std::string best_lever =
        r.has_whatif && !r.whatif.results.empty() && lever_pct >= 1.0
            ? " Measured best lever: " + r.whatif.results.front().change + " at '" + r.whatif.results.front().node + "' (+" +
                  f(r.whatif.results.front().delta_pct, 1) + "% throughput)."
            : "";

    // Integrity
    if (s.deadlock) {
        add(Severity::Critical, "Integrity", s.deadlock->cycle.empty() ? "pipeline" : s.deadlock->cycle,
            "The pipeline deadlocks at t=" + f(to_seconds(s.deadlock->at), 0) + " s: every remaining node waits on another" +
                (s.deadlock->cycle.empty() ? "." : " (wait-for cycle: " + s.deadlock->cycle + ")."),
            "Break the circular wait: put \"bypass\": true on the rework edge, give one node in the loop an unbounded buffer, or route rework to its own lane.",
            40);
    } else {
        for (const std::string& w : s.warnings)
            add(Severity::High, "Integrity", "loop", w,
                "Add a bypass lane or an unbounded buffer to the loop before relying on this design under heavy load.", 12);
    }
    if (!s.conservation_ok)
        add(Severity::Critical, "Integrity", "unit accounting",
            "Units created do not equal completed + scrapped + lost + in-system, so the model is losing track of parts.",
            "Do not trust these results. Check custom nodes for parts that are created or destroyed without being counted.", 30);

    // Flow and capacity
    const NodeReport* bn = nullptr;
    for (const auto& n : s.nodes)
        if (n.id == s.bottleneck) bn = &n;
    if (s.lost_pct >= 5.0)
        add(Severity::High, "Flow", "arrivals", f(s.lost_pct, 1) + "% of arrivals are lost because a buffer is full.",
            "Add capacity at the constraint or enlarge the buffer in front of it." + best_lever, static_cast<int>(std::min(15.0, s.lost_pct / 2.0 + 4)));
    else if (s.lost_pct >= 1.0)
        add(Severity::Medium, "Flow", "arrivals", f(s.lost_pct, 1) + "% of arrivals are lost.",
            "Check whether the loss is acceptable for this process." + best_lever, 5);
    if (s.sources_blocked_pct >= 25.0)
        add(Severity::Medium, "Flow", "supply", "Upstream supply is held back up to " + f(s.sources_blocked_pct, 0) + "% of the time by a full line.",
            "The line cannot absorb the offered supply, so everything upstream of the constraint spends time waiting.", 5);
    if (bn) {
        const double util = bn->pct[1] + bn->pct[3];
        if (util >= 95.0)
            add(Severity::High, "Capacity", bn->id, "The bottleneck is " + f(util, 0) + "% utilised (busy or down). There is no slack: any disturbance directly cuts output.",
                "Add parallel capacity, speed the station up, or reduce downtime." + best_lever, 8);
        else if (util >= 85.0)
            add(Severity::Medium, "Capacity", bn->id, "The bottleneck is " + f(util, 0) + "% utilised. Limited slack for demand growth or disturbances.",
                "Plan capacity before demand grows further." + best_lever, 4);
    }
    int blocked_penalty = 0;
    for (const auto& n : s.nodes) {
        if (!n.station) continue;
        if (n.pct[2] >= 15.0) {
            const int p = blocked_penalty < 9 ? 3 : 0;
            blocked_penalty += p;
            add(Severity::Medium, "Flow", n.id, "Blocked " + f(n.pct[2], 0) + "% of the time: finished parts cannot leave because the next buffer is full.",
                "Speed up or add capacity downstream of this station, or enlarge the buffer after it.", p);
        }
    }
    int idle_penalty = 0;
    for (const auto& n : s.nodes) {
        if (!n.station || s.demand_limited || (n.kind != "machine" && n.kind != "quality_gate")) continue;
        if (n.pct[1] + n.pct[3] < 15.0 && n.pct[0] > 80.0 && idle_penalty < 3) {
            ++idle_penalty;
            add(Severity::Low, "Capacity", n.id, "Only " + f(n.pct[1], 0) + "% busy: this station is over-provisioned for the current flow.",
                "Consider a smaller or shared resource here.", 1);
        }
    }
    if (s.little.gap_pct > 10.0)
        add(Severity::Medium, "Model", "steady state",
            "Little's Law differs by " + f(s.little.gap_pct, 1) + "%: the run has not reached steady state (units are still accumulating).",
            "Lengthen the run or use a warm-up period before trusting averages.", 4);

    // Buffers
    int unbounded_penalty = 0;
    std::vector<std::string> unbounded;
    for (const auto& n : s.nodes)
        if (n.station && n.capacity < 0) unbounded.push_back(n.id);
    for (const std::string& id : unbounded) {
        const int p = unbounded_penalty < 6 ? 2 : 0;
        unbounded_penalty += p;
        add(Severity::Medium, "Capacity", id, "Unbounded buffer: work-in-process can grow without limit if this station falls behind.",
            "Set a finite capacity so a slow station back-pressures its supplier instead of hiding the problem.", p);
    }

    // Reliability
    int down_penalty = 0;
    for (const auto& n : s.nodes)
        if (n.station && n.pct[3] >= 10.0) {
            const int p = down_penalty < 9 ? 3 : 0;
            down_penalty += p;
            add(Severity::Medium, "Reliability", n.id, "Down " + f(n.pct[3], 0) + "% of the time (" + std::to_string(n.failures) + " failures).",
                "Shorten repairs (spares, on-call technicians) or introduce preventive maintenance. A 20% shorter repair time is a what-if lever.", p);
        }
    std::vector<std::string> spof_failing;
    for (const auto& n : s.nodes) {
        if (!n.station) continue;
        const auto id = plant->find(n.id);
        if (id && is_single_point(*plant, *id)) {
            r.single_points_of_failure.push_back(n.id);
            if (n.mtbf_s > 0) spof_failing.push_back(n.id);
        }
    }
    if (!spof_failing.empty()) {
        std::string list;
        for (const auto& id : spof_failing) list += (list.empty() ? "" : ", ") + id;
        add(Severity::Medium, "Reliability", list,
            "Every unit passes through " + std::string(spof_failing.size() > 1 ? "these stations" : "this station") +
                (spof_failing.size() > 1 ? ", and they can fail." : ", and it can fail.") + " A single failure stops the whole plant.",
            "Add a parallel machine or a buffer of finished work in front of the downstream stations so they can ride through a repair.",
            std::min(12, 4 * static_cast<int>(spof_failing.size())));
    }
    const bool any_failures = std::any_of(r.inventory.begin(), r.inventory.end(), [](const InventoryRow& i) { return i.mtbf_s > 0; });
    if (!any_failures)
        add(Severity::Low, "Reliability", "failure model", "No machine has a failure model (MTBF/MTTR), so the results are optimistic.",
            "Add realistic MTBF and MTTR to the stations that break down in practice.", 3);

    // Confidence
    if (r.has_replication) {
        const double rel = r.replication.uph.mean > 0 ? 100.0 * r.replication.uph.ci95 / r.replication.uph.mean : 0;
        if (r.replication.deadlocks > 0 && !s.deadlock)
            add(Severity::High, "Integrity", "random seeds",
                std::to_string(r.replication.deadlocks) + " of " + std::to_string(r.replication.n) + " replications deadlock, although the reference run did not.",
                "The design is deadlock-prone: it only works for lucky random sequences. Add slack to any loop (bypass lane or unbounded buffer).", 15);
        if (rel > 10.0)
            add(Severity::Medium, "Confidence", "throughput", "Throughput varies strongly between runs: 95% CI is +-" + f(rel, 1) + "% of the mean over " +
                                                               std::to_string(r.replication.n) + " runs.",
                "Run longer, use a warm-up period, or reduce variability at the bottleneck.", 4);
        else if (rel > 5.0)
            add(Severity::Low, "Confidence", "throughput", "Throughput 95% CI is +-" + f(rel, 1) + "% of the mean.",
                "Use a longer run if you need tighter estimates.", 1);
    }
    const double window_s = s.window_s;
    if (s.cycle_time_s > 0 && window_s < 30.0 * s.cycle_time_s)
        add(Severity::Low, "Model", "run length", "The measured window (" + f(window_s / 3600.0, 1) + " h) is only " + f(window_s / s.cycle_time_s, 0) + "x the cycle time.",
            "Aim for at least 30x the cycle time so averages settle.", 2);

    // Information (no penalty)
    if (bn && !s.deadlock)
        add(Severity::Info, "Capacity", bn->id,
            s.demand_limited ? "Highest utilisation in the line (" + f(bn->pct[1] + bn->pct[3], 0) + "%); the line currently keeps up with demand."
                             : "Constraint of the line: " + f(bn->pct[1] + bn->pct[3], 0) + "% utilised.",
            s.demand_limited ? "This is where capacity runs out first as demand grows." : "Improvements elsewhere will not raise throughput until this station is relieved." + best_lever, 0);
    for (const auto& n : s.nodes)
        if (n.lost_units > 0 && n.kind != "source")
            add(Severity::Info, "Flow", n.id, std::to_string(n.lost_units) + " units were dropped here (drop-on-full policy).",
                "Intentional for walk-outs and abandoned calls; otherwise switch to blocking.", 0);
    if (!r.single_points_of_failure.empty() && spof_failing.empty())
        add(Severity::Info, "Reliability", "dependencies", std::to_string(r.single_points_of_failure.size()) + " stations lie on every path (serial structure), but none has a failure model.",
            "Fine for an ideal analysis; add failure data to find out how fragile this is.", 0);

    std::stable_sort(r.findings.begin(), r.findings.end(), [](const Finding& a, const Finding& b) { return a.severity < b.severity; });

    // ---- score ----
    int penalty = 0;
    for (const auto& fnd : r.findings) penalty += fnd.penalty;
    r.score = std::max(0, 100 - penalty);
    r.grade = r.score >= 90 ? 'A' : r.score >= 80 ? 'B' : r.score >= 70 ? 'C' : r.score >= 60 ? 'D' : 'F';
    if (s.deadlock) r.verdict = "Not fit for use: the pipeline deadlocks.";
    else if (r.score >= 90) r.verdict = "Healthy: the design is robust and has headroom.";
    else if (r.score >= 80) r.verdict = "Good: sound design with minor weaknesses worth addressing.";
    else if (r.score >= 70) r.verdict = "Acceptable: workable, but there are clear risks to address before scaling up.";
    else if (r.score >= 60) r.verdict = "At risk: significant constraints or fragility; fix the high-severity findings first.";
    else r.verdict = "Unhealthy: the design has serious problems that will limit or stop production.";
    return r;
}

json to_json(const AuditReport& r) {
    json findings = json::array();
    for (const auto& f_ : r.findings)
        findings.push_back({{"severity", to_string(f_.severity)}, {"area", f_.area}, {"subject", f_.subject},
                            {"issue", f_.issue}, {"recommendation", f_.recommendation}, {"penalty", f_.penalty}});
    json inv = json::array();
    for (const auto& i : r.inventory)
        inv.push_back({{"id", i.id}, {"label", i.label}, {"type", i.type}, {"params", i.params}, {"availability", i.availability}});
    json out = {{"title", r.title}, {"generated", r.generated}, {"score", r.score}, {"grade", std::string(1, r.grade)},
                {"verdict", r.verdict}, {"summary", to_json(r.summary)}, {"findings", findings}, {"inventory", inv},
                {"single_points_of_failure", r.single_points_of_failure}};
    if (r.has_replication) out["replication"] = to_json(r.replication);
    if (r.has_whatif) out["whatif"] = to_json(r.whatif);
    return out;
}

std::string audit_text_summary(const AuditReport& r) {
    std::ostringstream o;
    int counts[5] = {};
    for (const auto& f_ : r.findings) ++counts[static_cast<int>(f_.severity)];
    o << "Pipeline: " << r.title << "\n";
    o << "Health score: " << r.score << "/100 (grade " << r.grade << ")  " << r.verdict << "\n";
    o << "Findings: " << counts[0] << " critical, " << counts[1] << " high, " << counts[2] << " medium, " << counts[3] << " low, "
      << counts[4] << " info\n";
    int shown = 0;
    for (const auto& f_ : r.findings) {
        if (f_.severity == Severity::Info || shown >= 6) continue;
        o << "  [" << to_string(f_.severity) << "] " << f_.subject << ": " << f_.issue << "\n";
        ++shown;
    }
    return o.str();
}

}  // namespace factory
