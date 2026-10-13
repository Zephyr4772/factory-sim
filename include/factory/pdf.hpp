#pragma once
// A tiny dependency-free PDF writer: multi-page A4 documents with text (standard Helvetica /
// Courier fonts, no embedding), filled and stroked rectangles and lines. Enough for reports.
#include <string>
#include <string_view>
#include <vector>

namespace factory::pdf {

struct Color {
    double r = 0, g = 0, b = 0;
};
[[nodiscard]] constexpr Color rgb(int hex) noexcept {
    return {((hex >> 16) & 0xFF) / 255.0, ((hex >> 8) & 0xFF) / 255.0, (hex & 0xFF) / 255.0};
}

enum class Font { Regular, Bold, Mono };

inline constexpr double kPageWidth = 595.28;   // A4, points
inline constexpr double kPageHeight = 841.89;

// Non-ASCII characters are not representable in the base fonts: replace them with '?'.
[[nodiscard]] std::string sanitize(std::string_view text);
[[nodiscard]] double text_width(std::string_view text, Font font, double size);
// Greedy word wrap to `max_width` points.
[[nodiscard]] std::vector<std::string> wrap(std::string_view text, Font font, double size, double max_width);

class Document {
public:
    Document() = default;

    std::size_t new_page();  // returns the page index
    [[nodiscard]] std::size_t page_count() const noexcept { return pages_.size(); }

    // Coordinates are in points with the origin at the TOP-LEFT of the page; y grows downward.
    void rect(double x, double y, double w, double h, Color fill);
    void stroke_rect(double x, double y, double w, double h, Color stroke, double line_width = 0.75);
    void line(double x1, double y1, double x2, double y2, Color stroke, double line_width = 0.75);
    // `y` is the text baseline.
    void text(double x, double y, std::string_view s, Font font, double size, Color color);
    void text_on_page(std::size_t page, double x, double y, std::string_view s, Font font, double size, Color color);

    void set_title(std::string title) { title_ = std::move(title); }
    [[nodiscard]] std::string finish() const;  // the complete PDF file

private:
    std::vector<std::string> pages_;
    std::string title_ = "Document";
};

}  // namespace factory::pdf
