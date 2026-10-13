#include <filesystem>

#include "factory/audit.hpp"
#include "factory/pdf.hpp"
#include "test_util.hpp"

using namespace testutil;

namespace {
bool contains(const std::string& hay, const std::string& needle) { return hay.find(needle) != std::string::npos; }
std::size_t count(const std::string& hay, const std::string& needle) {
    std::size_t n = 0;
    for (std::size_t pos = hay.find(needle); pos != std::string::npos; pos = hay.find(needle, pos + 1)) ++n;
    return n;
}
bool has_finding(const AuditReport& r, Severity s) {
    for (const auto& f : r.findings)
        if (f.severity == s) return true;
    return false;
}
AuditOptions quick() {
    AuditOptions o;
    o.replications = 4;
    o.whatif_replications = 2;
    return o;
}
}  // namespace

TEST_CASE("pdf writer: structure, escaping and wrapping") {
    factory::pdf::Document d;
    d.set_title("T (test)");
    d.text(50, 100, "Hello (world) \\ back", factory::pdf::Font::Regular, 12, factory::pdf::rgb(0));
    d.new_page();
    d.rect(10, 10, 100, 20, factory::pdf::rgb(0xFF0000));
    const std::string out = d.finish();
    CHECK(out.rfind("%PDF-1.4", 0) == 0);
    CHECK(contains(out, "Hello \\(world\\) \\\\ back"));
    CHECK(contains(out, "/Count 2"));
    CHECK(out.substr(out.size() - 6) == "%%EOF\n");
    // startxref must point at the xref table
    const auto sx = out.rfind("startxref\n");
    REQUIRE(sx != std::string::npos);
    const auto offset = static_cast<std::size_t>(std::stoul(out.substr(sx + 10)));
    CHECK(out.compare(offset, 4, "xref") == 0);

    const auto lines = factory::pdf::wrap("the quick brown fox jumps over the lazy dog", factory::pdf::Font::Regular, 10, 80);
    CHECK(lines.size() > 1);
    for (const auto& l : lines) CHECK(factory::pdf::text_width(l, factory::pdf::Font::Regular, 10) <= 80.5);
    CHECK(factory::pdf::sanitize("caf\xC3\xA9") == "caf?");
}

TEST_CASE("audit of a healthy line scores well and produces a valid multi-page PDF") {
    const json cfg = J(R"({"name":"Simple line","simulation":{"duration_seconds":40000,"warmup_seconds":2000,"random_seed":3,"sample_interval":60},
      "nodes":[
        {"id":"src","type":"source","interarrival":{"dist":"exponential","mean":30},"downstream":["a"]},
        {"id":"a","type":"machine","capacity":8,"process":{"dist":"normal","mean":12,"stddev":2},"mtbf":5000,"mttr":200,"downstream":["b"]},
        {"id":"b","type":"machine","capacity":8,"process":{"dist":"normal","mean":10,"stddev":2},"downstream":["out"]},
        {"id":"out","type":"sink"}]})");
    const AuditReport r = audit(cfg, registry(), quick());
    CHECK(r.score >= 70);
    CHECK(r.score <= 100);
    CHECK_FALSE(has_finding(r, Severity::Critical));
    CHECK(r.has_replication);
    CHECK(r.has_whatif);
    const std::string pdf = render_pdf(r);
    CHECK(pdf.rfind("%PDF-1.4", 0) == 0);
    CHECK(pdf.substr(pdf.size() - 6) == "%%EOF\n");
    CHECK(count(pdf, "/Type /Page /Parent") >= 3);
    CHECK(contains(pdf, "Pipeline Audit and Health Report") == false);  // titles are drawn in capitals
    CHECK(contains(pdf, "PIPELINE AUDIT AND HEALTH REPORT"));
    CHECK(contains(pdf, "Simple line"));
}

TEST_CASE("a deadlocking pipeline gets a critical finding and a failing score") {
    json cfg = load_example("deadlock_demo.json");
    cfg["simulation"]["duration_seconds"] = 100000;
    const AuditReport r = audit(cfg, registry(), quick());
    CHECK(has_finding(r, Severity::Critical));
    CHECK(r.score < 60);
    CHECK(r.grade == 'F');
    CHECK(contains(r.verdict, "deadlock"));
    CHECK(contains(render_pdf(r), "Critical"));
}

TEST_CASE("single points of failure and unbounded buffers are reported") {
    const json cfg = J(R"({"name":"Fragile","simulation":{"duration_seconds":30000,"random_seed":2,"sample_interval":0},
      "nodes":[
        {"id":"src","type":"source","interarrival":20,"downstream":["only"]},
        {"id":"only","type":"machine","capacity":-1,"process":10,"mtbf":1000,"mttr":300,"downstream":["out"]},
        {"id":"out","type":"sink"}]})");
    const AuditReport r = audit(cfg, registry(), quick());
    bool spof = false, unbounded = false;
    for (const auto& f : r.findings) {
        spof |= f.area == "Reliability" && contains(f.issue, "Every unit passes through");
        unbounded |= contains(f.issue, "Unbounded buffer");
    }
    CHECK(spof);
    CHECK(unbounded);
    CHECK(r.single_points_of_failure == std::vector<std::string>{"only"});
}

TEST_CASE("parallel stations are not single points of failure") {
    const AuditReport r = audit(load_example("sample_factory.json"), registry(), quick());
    for (const auto& id : r.single_points_of_failure) {
        CHECK(id != "asm_a");
        CHECK(id != "asm_b");
    }
}

TEST_CASE("every shipped example can be audited into a valid PDF") {
    namespace fs = std::filesystem;
    for (const auto& entry : fs::directory_iterator(FACTORY_EXAMPLES_DIR)) {
        if (entry.path().extension() != ".json") continue;
        const std::string name = entry.path().filename().string();
        INFO(name);
        AuditOptions o;
        o.replications = 2;
        o.whatif_replications = 1;
        const AuditReport r = audit(load_example(name.c_str()), registry(), o);
        CHECK(r.score >= 0);
        CHECK(r.score <= 100);
        const std::string pdf = render_pdf(r);
        CHECK(pdf.rfind("%PDF-1.4", 0) == 0);
        CHECK(count(pdf, "/Type /Page /Parent") >= 3);
        CHECK(to_json(r)["findings"].is_array());
    }
}
