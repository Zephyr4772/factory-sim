#pragma once
// Audit and health report for a proposed pipeline.
//
// Combines (1) a full simulation run, (2) independent replications, (3) a what-if sweep and
// (4) static checks of the configuration into a list of findings, a 0-100 health score and a
// PDF document. The scoring is deliberately simple and fully listed in the report appendix:
// 100 minus the penalty of each finding.
#include <string>
#include <vector>

#include "factory/analytics.hpp"

namespace factory {

enum class Severity { Critical, High, Medium, Low, Info };
[[nodiscard]] const char* to_string(Severity s) noexcept;

struct Finding {
    Severity severity = Severity::Info;
    std::string area;  // Integrity | Flow | Reliability | Capacity | Confidence | Model
    std::string subject;
    std::string issue;
    std::string recommendation;
    int penalty = 0;   // points deducted from the health score
};

struct InventoryRow {
    std::string id, label, type, base_type;
    std::string params;           // compact "key=value" list of the effective parameters
    double mtbf_s = 0, mttr_s = 0;
    double availability = -1;     // mtbf/(mtbf+mttr); -1 when the node never fails
};

struct AuditOptions {
    int replications = 10;
    int whatif_replications = 3;
    bool include_whatif = true;
};

struct AuditReport {
    std::string title, description, generated;
    double duration_s = 0, warmup_s = 0;
    std::uint64_t seed = 0;
    int replications = 0;

    Summary summary;
    ReplicationReport replication;
    bool has_replication = false;
    WhatIfReport whatif;
    bool has_whatif = false;

    std::vector<Finding> findings;  // most severe first
    std::vector<std::string> single_points_of_failure;
    std::vector<InventoryRow> inventory;

    int score = 100;
    char grade = 'A';
    std::string verdict;
};

[[nodiscard]] AuditReport audit(const json& config, const NodeRegistry& registry, const AuditOptions& options = {});
[[nodiscard]] json to_json(const AuditReport& report);
[[nodiscard]] std::string render_pdf(const AuditReport& report);  // the PDF file as bytes
[[nodiscard]] std::string audit_text_summary(const AuditReport& report);

}  // namespace factory
