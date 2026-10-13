// Lays out the AuditReport as a multi-page A4 PDF using the small writer in pdf.cpp.
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <set>
#include <sstream>

#include "factory/audit.hpp"
#include "factory/pdf.hpp"

namespace factory {

namespace {

using pdf::Color;
using pdf::Font;
using pdf::rgb;

constexpr double kLeft = 48, kRight = pdf::kPageWidth - 48, kWidth = kRight - kLeft;
constexpr double kTop = 78, kBottom = pdf::kPageHeight - 62;

const Color kInk = rgb(0x1B2129), kMuted = rgb(0x667180), kAccent = rgb(0x2563D6), kRule = rgb(0xD7DCE3), kPanel = rgb(0xF3F4F6),
            kWhite = rgb(0xFFFFFF), kGood = rgb(0x1F9461), kWarn = rgb(0xC98A14), kBad = rgb(0xD03A37), kIdle = rgb(0x8A94A3);

Color severity_color(Severity s) {
    switch (s) {
        case Severity::Critical: return rgb(0xB42318);
        case Severity::High: return rgb(0xD03A37);
        case Severity::Medium: return rgb(0xC98A14);
        case Severity::Low: return rgb(0x2E6FB7);
        case Severity::Info: return rgb(0x667180);
    }
    return kMuted;
}
Color grade_color(char g) {
    switch (g) {
        case 'A': return rgb(0x1F9461);
        case 'B': return rgb(0x4C9A2A);
        case 'C': return rgb(0xC98A14);
        case 'D': return rgb(0xD9730D);
        default: return rgb(0xB42318);
    }
}

std::string f(double v, int prec = 1) {
    std::ostringstream os;
    os << std::fixed << std::setprecision(prec) << v;
    return os.str();
}

struct Cell {
    std::string text;
    std::string sub;                 // second line in muted text
    Color color = kInk;
    bool bold = false;
    bool badge = false;              // white text on a filled rectangle
    Color fill = kMuted;
    std::vector<std::pair<double, Color>> bar;  // stacked percentage bar instead of text
};
struct Col {
    std::string head;
    double width;
    bool right = false;
};

class Layout {
public:
    Layout(pdf::Document& doc, std::string title) : d_(doc), title_(std::move(title)) {}

    double y = 0;

    void new_page(bool header = true) {
        d_.new_page();
        if (header) {
            d_.text(kLeft, 40, "PIPELINE AUDIT AND HEALTH REPORT", Font::Bold, 7.5, kMuted);
            const std::string t = title_.size() > 60 ? title_.substr(0, 57) + "..." : title_;
            d_.text(kRight - pdf::text_width(t, Font::Regular, 7.5), 40, t, Font::Regular, 7.5, kMuted);
            d_.line(kLeft, 47, kRight, 47, kRule);
        }
        y = kTop;
    }
    void ensure(double h) {
        if (y + h > kBottom) new_page();
    }
    void gap(double h) { y += h; }

    void heading(const std::string& text) {
        ensure(130);  // heading + intro + first table rows stay together
        gap(10);
        d_.text(kLeft, y + 12, text, Font::Bold, 14, kInk);
        d_.line(kLeft, y + 20, kRight, y + 20, kAccent, 1.2);
        y += 32;
    }
    void subheading(const std::string& text) {
        ensure(30);
        d_.text(kLeft, y + 10, text, Font::Bold, 10.5, kInk);
        y += 20;
    }
    void paragraph(const std::string& text, double size = 9.5, Color color = kInk, Font font = Font::Regular, double indent = 0,
                   double after = 6) {
        const double lh = size * 1.45;
        for (const std::string& line : pdf::wrap(text, font, size, kWidth - indent)) {
            ensure(lh);
            d_.text(kLeft + indent, y + size, line, font, size, color);
            y += lh;
        }
        y += after;
    }
    void bullet(const std::string& text, double size = 9.5) {
        const double lh = size * 1.45, indent = 14;
        const auto lines = pdf::wrap(text, Font::Regular, size, kWidth - indent);
        ensure(lh);
        d_.rect(kLeft + 3, y + size * 0.55, 3, 3, kAccent);
        for (const std::string& line : lines) {
            ensure(lh);
            d_.text(kLeft + indent, y + size, line, Font::Regular, size, kInk);
            y += lh;
        }
        y += 3;
    }

    void table(const std::vector<Col>& cols, const std::vector<std::vector<Cell>>& rows, double size = 8.5) {
        const double pad = 5, lh = size * 1.4;
        auto draw_header = [&] {
            ensure(24);
            d_.rect(kLeft, y, kWidth, 20, kPanel);
            double x = kLeft;
            for (const Col& c : cols) {
                const double tw = pdf::text_width(c.head, Font::Bold, 7.5);
                d_.text(c.right ? x + c.width - pad - tw : x + pad, y + 13.5, c.head, Font::Bold, 7.5, kMuted);
                x += c.width;
            }
            y += 20;
        };
        ensure(70);
        draw_header();
        for (const auto& row : rows) {
            // measure
            std::vector<std::vector<std::string>> main(cols.size()), sub(cols.size());
            double h = 0;
            for (std::size_t i = 0; i < cols.size(); ++i) {
                const Cell& c = row[i];
                const double w = cols[i].width - 2 * pad;
                double ch = 0;
                if (c.bar.empty()) {
                    main[i] = pdf::wrap(c.text, c.bold || c.badge ? Font::Bold : Font::Regular, size, w);
                    ch += static_cast<double>(main[i].size()) * lh;
                    if (!c.sub.empty()) {
                        sub[i] = pdf::wrap(c.sub, Font::Regular, size - 0.7, w);
                        ch += static_cast<double>(sub[i].size()) * (lh - 0.8);
                    }
                } else {
                    ch = 10;
                }
                h = std::max(h, ch);
            }
            h += 2 * pad - 1;
            if (y + h > kBottom) {
                new_page();
                draw_header();
            }
            double x = kLeft;
            for (std::size_t i = 0; i < cols.size(); ++i) {
                const Cell& c = row[i];
                const Col& col = cols[i];
                double ty = y + pad + size;
                if (!c.bar.empty()) {
                    const double bw = col.width - 2 * pad;
                    d_.rect(x + pad, y + pad + 1, bw, 8, kRule);
                    double off = 0;
                    for (const auto& [pct, colr] : c.bar) {
                        const double w = bw * std::clamp(pct, 0.0, 100.0) / 100.0;
                        if (w > 0.2) d_.rect(x + pad + off, y + pad + 1, w, 8, colr);
                        off += w;
                    }
                } else if (c.badge) {
                    const double tw = pdf::text_width(c.text, Font::Bold, size) + 10;
                    d_.rect(x + pad, y + pad - 1, tw, lh + 1, c.fill);
                    d_.text(x + pad + 5, ty, c.text, Font::Bold, size, kWhite);
                } else {
                    for (const std::string& line : main[i]) {
                        const double tw = pdf::text_width(line, c.bold ? Font::Bold : Font::Regular, size);
                        d_.text(col.right ? x + col.width - pad - tw : x + pad, ty, line, c.bold ? Font::Bold : Font::Regular, size, c.color);
                        ty += lh;
                    }
                    for (const std::string& line : sub[i]) {
                        d_.text(x + pad, ty, line, Font::Regular, size - 0.7, kMuted);
                        ty += lh - 0.8;
                    }
                }
                x += col.width;
            }
            y += h;
            d_.line(kLeft, y, kRight, y, kRule, 0.5);
        }
        y += 8;
    }

    void line_chart(const std::vector<std::pair<double, double>>& pts, const std::string& x_label, const std::string& y_label, Color color) {
        const double h = 130, w = kWidth, left = 36;
        ensure(h + 34);
        const double top = y + 6;
        d_.rect(kLeft, top, w, h, kPanel);
        if (pts.size() < 2) {
            d_.text(kLeft + 10, top + 20, "No telemetry samples (sample interval is 0).", Font::Regular, 9, kMuted);
            y += h + 14;
            return;
        }
        double tmax = pts.back().first, vmax = 1;
        for (const auto& p : pts) vmax = std::max(vmax, p.second);
        vmax *= 1.1;
        for (int i = 0; i <= 4; ++i) {
            const double gy = top + 8 + (h - 22) * i / 4.0;
            d_.line(kLeft + left, gy, kLeft + w - 8, gy, kRule, 0.4);
            d_.text(kLeft + 4, gy + 3, f(vmax * (1.0 - i / 4.0), 0), Font::Regular, 7, kMuted);
        }
        auto X = [&](double t) { return kLeft + left + (w - left - 10) * t / tmax; };
        auto Y = [&](double v) { return top + 8 + (h - 22) * (1.0 - v / vmax); };
        const std::size_t step = std::max<std::size_t>(1, pts.size() / 400);
        for (std::size_t i = step; i < pts.size(); i += step)
            d_.line(X(pts[i - step].first), Y(pts[i - step].second), X(pts[i].first), Y(pts[i].second), color, 1.2);
        d_.text(kLeft + left, top + h - 3, "0", Font::Regular, 7, kMuted);
        const std::string end = f(tmax / 3600.0, 1) + " h";
        d_.text(kLeft + w - 10 - pdf::text_width(end, Font::Regular, 7), top + h - 3, end, Font::Regular, 7, kMuted);
        d_.text(kLeft + left + 6, top + 14, y_label + " over time (" + x_label + ")", Font::Bold, 7.5, kMuted);
        y += h + 14;
    }

    void finish_footers(const std::string& generated) {
        const std::size_t n = d_.page_count();
        for (std::size_t i = 0; i < n; ++i) {
            const std::string right = "Page " + std::to_string(i + 1) + " of " + std::to_string(n);
            d_.text_on_page(i, kLeft, pdf::kPageHeight - 34, "Generated " + generated + " by factory_sim", Font::Regular, 7.5, kMuted);
            d_.text_on_page(i, kRight - pdf::text_width(right, Font::Regular, 7.5), pdf::kPageHeight - 34, right, Font::Regular, 7.5, kMuted);
            d_.line(kLeft, pdf::kPageHeight - 44, kRight, pdf::kPageHeight - 44, kRule, 0.5);
        }
    }

    pdf::Document& doc() { return d_; }

private:
    pdf::Document& d_;
    std::string title_;
};

Cell txt(std::string t, bool bold = false, Color c = kInk) {
    Cell cell;
    cell.text = std::move(t);
    cell.bold = bold;
    cell.color = c;
    return cell;
}

}  // namespace

std::string render_pdf(const AuditReport& r) {
    pdf::Document doc;
    doc.set_title("Pipeline audit: " + r.title);
    Layout L(doc, r.title);
    const Summary& s = r.summary;

    // ============================================================ cover / summary
    L.new_page(false);
    doc.text(kLeft, 56, "PIPELINE AUDIT AND HEALTH REPORT", Font::Bold, 9, kAccent);
    L.y = 66;
    for (const std::string& line : pdf::wrap(r.title, Font::Bold, 22, kWidth)) {
        doc.text(kLeft, L.y + 22, line, Font::Bold, 22, kInk);
        L.y += 28;
    }
    L.gap(4);
    if (!r.description.empty()) L.paragraph(r.description, 9, kMuted, Font::Regular, 0, 4);
    {
        std::ostringstream meta;
        meta << "Generated " << r.generated << "   |   simulated " << f(r.duration_s / 3600.0, 1) << " h";
        if (r.warmup_s > 0) meta << " (warm-up " << f(r.warmup_s / 60.0, 0) << " min)";
        meta << "   |   seed " << r.seed;
        if (r.has_replication) meta << "   |   " << r.replications << " replications";
        L.paragraph(meta.str(), 8.5, kMuted, Font::Regular, 0, 10);
    }

    // score card
    {
        L.ensure(120);
        const double top = L.y, cardW = 150, cardH = 104;
        const Color gc = grade_color(r.grade);
        doc.rect(kLeft, top, cardW, cardH, gc);
        doc.text(kLeft + 14, top + 20, "HEALTH SCORE", Font::Bold, 8, kWhite);
        const std::string score = std::to_string(r.score);
        doc.text(kLeft + 14, top + 66, score, Font::Bold, 44, kWhite);
        doc.text(kLeft + 14 + pdf::text_width(score, Font::Bold, 44) + 4, top + 66, "/100", Font::Regular, 12, kWhite);
        doc.text(kLeft + 14, top + 90, std::string("Grade ") + r.grade, Font::Bold, 13, kWhite);

        const double rx = kLeft + cardW + 20, rw = kWidth - cardW - 20;
        double ty = top;
        for (const std::string& line : pdf::wrap(r.verdict, Font::Bold, 12, rw)) {
            doc.text(rx, ty + 12, line, Font::Bold, 12, kInk);
            ty += 16;
        }
        ty += 6;
        int counts[5] = {};
        for (const auto& fd : r.findings) ++counts[static_cast<int>(fd.severity)];
        const Severity order[5] = {Severity::Critical, Severity::High, Severity::Medium, Severity::Low, Severity::Info};
        double cx = rx;
        for (int i = 0; i < 5; ++i) {
            const std::string label = std::string(to_string(order[i])) + " " + std::to_string(counts[i]);
            const double w = pdf::text_width(label, Font::Bold, 8) + 14;
            doc.rect(cx, ty, w, 16, counts[i] ? severity_color(order[i]) : kPanel);
            doc.text(cx + 7, ty + 11.5, label, Font::Bold, 8, counts[i] ? kWhite : kMuted);
            cx += w + 6;
        }
        ty += 28;
        for (const std::string& line : pdf::wrap(
                 "The score starts at 100 and loses points for each finding. The deductions are listed in Appendix B.", Font::Regular, 8.5, rw)) {
            doc.text(rx, ty + 8, line, Font::Regular, 8.5, kMuted);
            ty += 12;
        }
        L.y = top + cardH + 18;
    }

    // KPI tiles
    {
        const std::pair<std::string, std::string> tiles[6] = {
            {"THROUGHPUT", f(s.uph, 1) + " units/h"},       {"COMPLETED", std::to_string(s.total.completed) + " units"},
            {"ARRIVALS LOST", f(s.lost_pct, 1) + "%"},       {"AVERAGE WIP", f(s.avg_wip, 1) + " units"},
            {"CYCLE TIME", f(s.cycle_time_s, 0) + " s"},     {"BOTTLENECK", s.bottleneck.empty() ? "-" : s.bottleneck}};
        const double tw = (kWidth - 20) / 3.0, th = 46;
        L.ensure(2 * th + 20);
        for (int i = 0; i < 6; ++i) {
            const double x = kLeft + (i % 3) * (tw + 10), y = L.y + (i / 3) * (th + 10);
            doc.rect(x, y, tw, th, kPanel);
            doc.text(x + 12, y + 16, tiles[i].first, Font::Bold, 7.5, kMuted);
            std::string v = tiles[i].second;
            while (pdf::text_width(v, Font::Bold, 15) > tw - 24 && v.size() > 4) v = v.substr(0, v.size() - 4) + "...";
            doc.text(x + 12, y + 36, v, Font::Bold, 15, kInk);
        }
        L.y += 2 * th + 24;
    }

    L.heading("Executive summary");
    L.paragraph(s.diagnosis.empty() ? "No diagnosis available." : s.diagnosis);
    {
        std::vector<const Finding*> actionable;
        for (const auto& fd : r.findings)
            if (fd.severity != Severity::Info) actionable.push_back(&fd);
        if (actionable.empty()) {
            L.paragraph("No issues were found. The design is sound under the simulated demand.");
        } else {
            L.subheading("What to fix first");
            std::set<std::string> seen;
            int shown = 0;
            for (const Finding* fd : actionable) {
                if (shown >= 4) break;
                if (!seen.insert(fd->recommendation).second) continue;
                L.bullet(std::string(to_string(fd->severity)) + " (" + fd->subject + "): " + fd->recommendation);
                ++shown;
            }
        }
        if (r.has_whatif && !r.whatif.recommendation.empty()) {
            L.gap(2);
            L.subheading("Best measured improvement");
            L.paragraph(r.whatif.recommendation);
        }
    }

    // ============================================================ findings
    L.heading("Findings");
    L.paragraph("Each finding names where the problem is, why it matters, and what to do. Severity reflects the impact on throughput, correctness or confidence in the results.",
                9, kMuted);
    {
        std::vector<std::vector<Cell>> rows;
        for (const auto& fd : r.findings) {
            Cell sev;
            sev.text = to_string(fd.severity);
            sev.badge = true;
            sev.fill = severity_color(fd.severity);
            Cell what = txt(fd.issue);
            what.sub = "Action: " + fd.recommendation;
            rows.push_back({sev, txt(fd.area, false, kMuted), txt(fd.subject, true), what, txt(fd.penalty ? "-" + std::to_string(fd.penalty) : "", false, kMuted)});
        }
        L.table({{"SEVERITY", 62}, {"AREA", 58}, {"WHERE", 84}, {"ISSUE AND RECOMMENDED ACTION", 261}, {"POINTS", 34, true}}, rows);
    }

    // ============================================================ performance
    L.heading("Performance by node");
    L.paragraph("Share of the measured window each node spent busy, blocked (finished part cannot leave), down (failed) and starved (waiting for work).", 9, kMuted);
    L.paragraph("Legend: green = busy, amber = blocked, red = down, grey = starved or idle.", 8.5, kMuted, Font::Regular, 0, 6);
    {
        std::vector<std::vector<Cell>> rows;
        for (const auto& n : s.nodes) {
            Cell id = txt(n.id, true);
            if (n.id == s.bottleneck && !s.deadlock) id.sub = "bottleneck";
            Cell bar;
            if (n.kind != "source" && n.kind != "sink") bar.bar = {{n.pct[1], kGood}, {n.pct[2], kWarn}, {n.pct[3], kBad}, {n.pct[0], kIdle}};
            else bar.text = "-";
            const bool real = n.kind != "source" && n.kind != "sink";
            rows.push_back({id, txt(n.kind, false, kMuted), bar, txt(real ? f(n.pct[1], 1) : "-"), txt(real ? f(n.pct[2], 1) : "-"),
                            txt(real ? f(n.pct[3], 1) : "-"), txt(real ? f(n.avg_occupancy, 1) + " / " + std::to_string(n.max_occupancy) : "-")});
        }
        L.table({{"NODE", 100}, {"KIND", 66}, {"STATE MIX", 120}, {"BUSY %", 46, true}, {"BLOCKED %", 56, true}, {"DOWN %", 46, true}, {"AVG / MAX WIP", 65, true}}, rows);
    }
    {
        std::vector<std::pair<double, double>> pts;
        for (const auto& sm : s.samples) pts.emplace_back(to_seconds(sm.t), static_cast<double>(sm.wip));
        L.subheading("Work in process over time");
        L.line_chart(pts, "hours", "Units in the system", kAccent);
    }

    // ============================================================ reliability and robustness
    L.heading("Reliability and robustness");
    if (r.has_replication) {
        const ReplicationReport& rep = r.replication;
        L.paragraph("The pipeline was simulated " + std::to_string(rep.n) + " times with different random seeds. A wide interval means the result depends strongly on luck.", 9, kMuted);
        auto row = [&](const std::string& name, const Stat& st, const std::string& unit) {
            const double rel = st.mean != 0 ? 100.0 * st.ci95 / std::abs(st.mean) : 0;
            return std::vector<Cell>{txt(name, true), txt(f(st.mean, 2) + " " + unit), txt("+- " + f(st.ci95, 2)), txt(f(st.sd, 2)), txt(f(rel, 1) + "%")};
        };
        L.table({{"METRIC", 150}, {"MEAN", 110, true}, {"95% CONFIDENCE", 90, true}, {"STD DEV", 80, true}, {"RELATIVE", 69, true}},
                {row("Throughput", rep.uph, "units/h"), row("Average WIP", rep.wip, "units"), row("Cycle time", rep.cycle_time_s, "s"),
                 row("Arrivals lost", rep.lost_pct, "%")});
        L.paragraph(rep.deadlocks == 0 ? "No replication deadlocked." : std::to_string(rep.deadlocks) + " of " + std::to_string(rep.n) + " replications deadlocked.",
                    9, rep.deadlocks ? kBad : kGood, Font::Bold);
    } else {
        L.paragraph("Replications were not run for this report.", 9, kMuted);
    }
    {
        std::vector<std::vector<Cell>> rows;
        for (const auto& inv : r.inventory) {
            if (inv.mtbf_s <= 0) continue;
            const NodeReport* nr = nullptr;
            for (const auto& n : s.nodes)
                if (n.id == inv.id) nr = &n;
            rows.push_back({txt(inv.id, true), txt(f(inv.mtbf_s / 60.0, 0) + " min"), txt(f(inv.mttr_s / 60.0, 1) + " min"),
                            txt(inv.availability >= 0 ? f(100.0 * inv.availability, 1) + "%" : "-"), txt(nr ? f(nr->pct[3], 1) + "%" : "-"),
                            txt(nr ? std::to_string(nr->failures) : "-")});
        }
        L.subheading("Failure model");
        if (rows.empty()) L.paragraph("No machine has a failure model.", 9, kMuted);
        else L.table({{"NODE", 120}, {"MTBF", 70, true}, {"MTTR", 70, true}, {"AVAILABILITY", 90, true}, {"OBSERVED DOWN", 85, true}, {"FAILURES", 64, true}}, rows);
    }
    {
        L.subheading("Single points of failure");
        if (r.single_points_of_failure.empty()) {
            L.paragraph("Every station has a parallel alternative: no single station can stop the whole pipeline.", 9);
        } else {
            std::string list;
            for (const auto& id : r.single_points_of_failure) list += (list.empty() ? "" : ", ") + id;
            L.paragraph("All units must pass through: " + list + ". If one of these stops, output stops.", 9);
        }
    }

    // ============================================================ model validity
    L.heading("Model validity checks");
    {
        auto check = [&](const std::string& name, const std::string& result, const std::string& detail) {
            Cell res;
            res.text = result;
            res.badge = true;
            res.fill = result == "PASS" ? kGood : result == "WARN" ? kWarn : kBad;
            return std::vector<Cell>{txt(name, true), res, txt(detail)};
        };
        std::vector<std::vector<Cell>> rows;
        rows.push_back(check("Unit conservation", s.conservation_ok ? "PASS" : "FAIL",
                             "created " + std::to_string(s.total.created) + " = completed " + std::to_string(s.total.completed) + " + scrapped " +
                                 std::to_string(s.total.scrapped) + " + lost " + std::to_string(s.total.lost) + " + in system " + std::to_string(s.wip_end)));
        rows.push_back(check("Little's Law", s.little.gap_pct < 5 ? "PASS" : s.little.gap_pct < 10 ? "WARN" : "FAIL",
                             "L = " + f(s.little.L, 2) + ", lambda x W = " + f(s.little.lambda * s.little.W, 2) + " (gap " + f(s.little.gap_pct, 1) + "%)"));
        const double ratio = s.cycle_time_s > 0 ? s.window_s / s.cycle_time_s : 0;
        rows.push_back(check("Run length", ratio >= 30 ? "PASS" : "WARN", "window = " + f(ratio, 0) + " x cycle time (30x or more recommended)"));
        rows.push_back(check("Warm-up", r.warmup_s > 0 ? "PASS" : "WARN",
                             r.warmup_s > 0 ? "first " + f(r.warmup_s / 60.0, 0) + " min excluded from statistics" : "no warm-up: start-up transients are included in averages"));
        rows.push_back(check("Deadlock-free", s.deadlock ? "FAIL" : "PASS", s.deadlock ? "deadlock at t = " + f(to_seconds(s.deadlock->at), 0) + " s" : "reference run completed"));
        L.table({{"CHECK", 110}, {"RESULT", 70}, {"DETAIL", 319}}, rows);
    }

    // ============================================================ what-if
    L.heading("What-if analysis");
    if (r.has_whatif && !r.whatif.results.empty()) {
        L.paragraph("Each lever was applied to one of the most constrained stations and the whole pipeline re-simulated over at least 8 hours with " +
                        std::to_string(r.whatif.reps) + " identical random seeds per scenario. Baseline throughput: " + f(r.whatif.base_uph, 1) + " units/h.",
                    9, kMuted);
        std::vector<std::vector<Cell>> rows;
        for (const auto& w : r.whatif.results)
            rows.push_back({txt(w.node, true), txt(w.change), txt(f(w.uph, 1)), txt((w.delta >= 0 ? "+" : "") + f(w.delta, 1), true, w.delta >= 0.5 ? kGood : kInk),
                            txt((w.delta_pct >= 0 ? "+" : "") + f(w.delta_pct, 1) + "%")});
        L.table({{"NODE", 130}, {"CHANGE", 150}, {"UNITS/H", 70, true}, {"CHANGE", 75, true}, {"CHANGE %", 74, true}}, rows);
        L.paragraph(r.whatif.recommendation, 9.5, kInk, Font::Bold);
    } else {
        L.paragraph("What-if analysis was not run for this report.", 9, kMuted);
    }

    // ============================================================ appendices
    L.heading("Appendix A: node inventory");
    L.paragraph("Effective parameters after node-type templates are applied. Parameters not listed use the node type's defaults.", 9, kMuted);
    {
        std::vector<std::vector<Cell>> rows;
        for (const auto& inv : r.inventory) {
            Cell type = txt(inv.type);
            if (inv.base_type != inv.type) type.sub = "based on " + inv.base_type;
            rows.push_back({txt(inv.id, true), type, txt(inv.params, false, kInk)});
        }
        L.table({{"NODE", 100}, {"TYPE", 90}, {"EFFECTIVE PARAMETERS", 309}}, rows, 8);
    }
    L.heading("Appendix B: how the score is computed");
    L.paragraph("Health score = 100 minus the sum of the penalties below, never less than 0. Grades: A 90 and above, B 80 to 89, C 70 to 79, D 60 to 69, F below 60. "
                "Penalties are capped per category so that one repeated issue cannot dominate the score. Informational findings carry no penalty.",
                9);
    {
        std::vector<std::vector<Cell>> rows;
        int total = 0;
        for (const auto& fd : r.findings) {
            if (fd.penalty <= 0) continue;
            total += fd.penalty;
            Cell sev;
            sev.text = to_string(fd.severity);
            sev.badge = true;
            sev.fill = severity_color(fd.severity);
            rows.push_back({sev, txt(fd.subject, true), txt(fd.issue.size() > 110 ? fd.issue.substr(0, 107) + "..." : fd.issue), txt("-" + std::to_string(fd.penalty))});
        }
        rows.push_back({txt(""), txt("Total deductions", true), txt(""), txt("-" + std::to_string(total), true)});
        rows.push_back({txt(""), txt("Health score", true), txt(""), txt(std::to_string(r.score), true, grade_color(r.grade))});
        L.table({{"SEVERITY", 62}, {"WHERE", 100}, {"FINDING", 297}, {"POINTS", 40, true}}, rows);
    }
    L.paragraph("Method: the pipeline is simulated as a discrete-event system; the bottleneck index is utilisation (busy plus down time) with small weights for starved downstream and "
                "blocked upstream neighbours. What-if results are measured, not estimated. Confidence intervals use Student's t over independent replications.",
                8.5, kMuted);

    L.finish_footers(r.generated);
    return doc.finish();
}

}  // namespace factory
