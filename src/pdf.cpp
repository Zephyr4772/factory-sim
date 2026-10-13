#include "factory/pdf.hpp"

#include <cstdio>
#include <stdexcept>

namespace factory::pdf {

namespace {

// Glyph advance widths (1/1000 em) for ASCII 32..126, from the Adobe core-14 font metrics.
constexpr int kHelvetica[95] = {
    278, 278, 355, 556, 556, 889, 667, 191, 333, 333, 389, 584, 278, 333, 278, 278,  // space .. /
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556,                                // 0 .. 9
    278, 278, 584, 584, 584, 556, 1015,                                              // : .. @
    667, 667, 722, 722, 667, 611, 778, 722, 278, 500, 667, 556, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611,
    278, 278, 278, 469, 556, 333,                                                    // [ .. `
    556, 556, 500, 556, 556, 278, 556, 556, 222, 222, 500, 222, 833, 556, 556, 556, 556, 333, 500, 278, 556, 500, 722, 500, 500, 500,
    334, 260, 334, 584};
constexpr int kHelveticaBold[95] = {
    278, 333, 474, 556, 556, 889, 722, 238, 333, 333, 389, 584, 278, 333, 278, 278,
    556, 556, 556, 556, 556, 556, 556, 556, 556, 556,
    333, 333, 584, 584, 584, 611, 975,
    722, 722, 722, 722, 667, 611, 778, 722, 278, 556, 722, 611, 833, 722, 778, 667, 778, 722, 667, 611, 722, 667, 944, 667, 667, 611,
    333, 278, 333, 584, 556, 333,
    556, 611, 556, 611, 556, 333, 611, 611, 278, 278, 556, 278, 889, 611, 611, 611, 611, 389, 556, 333, 611, 556, 778, 556, 556, 500,
    389, 280, 389, 584};

const char* font_name(Font f) {
    switch (f) {
        case Font::Regular: return "F1";
        case Font::Bold: return "F2";
        case Font::Mono: return "F3";
    }
    return "F1";
}

std::string num(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.2f", v);
    return buf;
}

std::string escape(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '(' || c == ')' || c == '\\') out += '\\';
        out += c;
    }
    return out;
}

std::string color_op(Color c, const char* op) { return num(c.r) + " " + num(c.g) + " " + num(c.b) + " " + op + "\n"; }

}  // namespace

std::string sanitize(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c >= 32 && c < 127) out += ch;
        else if (c == '\t' || c == '\n' || c == '\r') out += ' ';
        else if (c >= 0x80 && (c & 0xC0) != 0x80) out += '?';  // one '?' per UTF-8 sequence
    }
    return out;
}

double text_width(std::string_view text, Font font, double size) {
    double w = 0;
    for (char ch : sanitize(text)) {
        const int idx = ch - 32;
        if (font == Font::Mono) w += 600;
        else w += (font == Font::Bold ? kHelveticaBold : kHelvetica)[idx];
    }
    return w * size / 1000.0;
}

std::vector<std::string> wrap(std::string_view text, Font font, double size, double max_width) {
    std::vector<std::string> lines;
    const std::string clean = sanitize(text);
    std::string line, word;
    auto flush_word = [&] {
        if (word.empty()) return;
        const std::string trial = line.empty() ? word : line + " " + word;
        if (!line.empty() && text_width(trial, font, size) > max_width) {
            lines.push_back(line);
            line = word;
        } else {
            line = trial;
        }
        // a single word wider than the column: hard-break it
        while (text_width(line, font, size) > max_width && line.size() > 1) {
            std::size_t cut = line.size();
            while (cut > 1 && text_width(line.substr(0, cut), font, size) > max_width) --cut;
            lines.push_back(line.substr(0, cut));
            line = line.substr(cut);
        }
        word.clear();
    };
    for (char c : clean) {
        if (c == ' ') flush_word();
        else word += c;
    }
    flush_word();
    if (!line.empty() || lines.empty()) lines.push_back(line);
    return lines;
}

std::size_t Document::new_page() {
    pages_.emplace_back();
    return pages_.size() - 1;
}

void Document::rect(double x, double y, double w, double h, Color fill) {
    if (pages_.empty()) new_page();
    pages_.back() += color_op(fill, "rg") + num(x) + " " + num(kPageHeight - y - h) + " " + num(w) + " " + num(h) + " re f\n";
}

void Document::stroke_rect(double x, double y, double w, double h, Color stroke, double lw) {
    if (pages_.empty()) new_page();
    pages_.back() += color_op(stroke, "RG") + num(lw) + " w " + num(x) + " " + num(kPageHeight - y - h) + " " + num(w) + " " +
                     num(h) + " re S\n";
}

void Document::line(double x1, double y1, double x2, double y2, Color stroke, double lw) {
    if (pages_.empty()) new_page();
    pages_.back() += color_op(stroke, "RG") + num(lw) + " w " + num(x1) + " " + num(kPageHeight - y1) + " m " + num(x2) + " " +
                     num(kPageHeight - y2) + " l S\n";
}

void Document::text_on_page(std::size_t page, double x, double y, std::string_view s, Font font, double size, Color color) {
    if (page >= pages_.size()) throw std::out_of_range("pdf page index");
    pages_[page] += "BT\n" + color_op(color, "rg") + "/" + font_name(font) + " " + num(size) + " Tf\n" + num(x) + " " +
                    num(kPageHeight - y) + " Td\n(" + escape(sanitize(s)) + ") Tj\nET\n";
}

void Document::text(double x, double y, std::string_view s, Font font, double size, Color color) {
    if (pages_.empty()) new_page();
    text_on_page(pages_.size() - 1, x, y, s, font, size, color);
}

std::string Document::finish() const {
    // Object layout: 1 catalog, 2 page tree, 3-5 fonts, 6 info, then (page, content) pairs.
    const std::size_t n_pages = pages_.empty() ? 1 : pages_.size();
    const std::size_t total = 6 + 2 * n_pages;
    std::vector<std::size_t> offset(total + 1, 0);
    std::string out = "%PDF-1.4\n%\xE2\xE3\xCF\xD3\n";
    auto object = [&](std::size_t id, const std::string& body) {
        offset[id] = out.size();
        out += std::to_string(id) + " 0 obj\n" + body + "\nendobj\n";
    };
    std::string kids;
    for (std::size_t i = 0; i < n_pages; ++i) kids += std::to_string(6 + 2 * i + 1) + " 0 R ";
    object(1, "<< /Type /Catalog /Pages 2 0 R >>");
    object(2, "<< /Type /Pages /Count " + std::to_string(n_pages) + " /Kids [ " + kids + "] >>");
    object(3, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>");
    object(4, "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>");
    object(5, "<< /Type /Font /Subtype /Type1 /BaseFont /Courier /Encoding /WinAnsiEncoding >>");
    object(6, "<< /Title (" + escape(sanitize(title_)) + ") /Producer (factory_sim) >>");
    for (std::size_t i = 0; i < n_pages; ++i) {
        const std::size_t page_id = 7 + 2 * i, content_id = page_id + 1;
        const std::string content = pages_.empty() ? std::string() : pages_[i];
        object(page_id, "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 " + num(kPageWidth) + " " + num(kPageHeight) +
                            "] /Resources << /Font << /F1 3 0 R /F2 4 0 R /F3 5 0 R >> >> /Contents " +
                            std::to_string(content_id) + " 0 R >>");
        object(content_id, "<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream");
    }
    const std::size_t xref_at = out.size();
    out += "xref\n0 " + std::to_string(total + 1) + "\n0000000000 65535 f \n";
    for (std::size_t id = 1; id <= total; ++id) {
        char buf[32];
        std::snprintf(buf, sizeof buf, "%010zu 00000 n \n", offset[id]);
        out += buf;
    }
    out += "trailer\n<< /Size " + std::to_string(total + 1) + " /Root 1 0 R /Info 6 0 R >>\nstartxref\n" +
           std::to_string(xref_at) + "\n%%EOF\n";
    return out;
}

}  // namespace factory::pdf
