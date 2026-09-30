#pragma once

#include "ui/font.h"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace frame_notify::ui {

enum class FontWeight {
    kRegular,
    kSemiBold,
};

struct TextStyle {
    float size = 16.0F;
    FontWeight weight = FontWeight::kRegular;
    float tracking = 0.0F;  // extra pixels between glyphs
};

// An 8-bit coverage bitmap for one glyph. `left` and `top` are pixel offsets from the pen
// position on the baseline, so `top` is negative for anything above the baseline.
struct GlyphImage {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> alpha;
};

struct PlacedGlyph {
    const GlyphImage* image;
    int x;  // integer pen position
};

// Loads the bundled Inter fonts and turns UTF-8 text into positioned, anti-aliased glyphs. When the
// font files cannot be found it falls back to a built-in 5x7 bitmap font, so text is always drawn.
// Not thread-safe: the glyph cache is filled lazily.
class Typography {
public:
    explicit Typography(const std::string& font_directory);

    // The process-wide instance; searches the usual locations for the bundled fonts once.
    [[nodiscard]] static const Typography& shared();
    [[nodiscard]] static std::string find_font_directory();

    [[nodiscard]] bool has_fonts() const noexcept { return regular_ != nullptr; }
    [[nodiscard]] float measure(std::string_view text, const TextStyle& style) const;
    [[nodiscard]] float ascent(const TextStyle& style) const;
    [[nodiscard]] float cap_height(const TextStyle& style) const;
    [[nodiscard]] std::vector<PlacedGlyph> layout(std::string_view text, const TextStyle& style,
                                                  float x) const;
    // Greedy word wrap on '\n' and spaces. Text that does not fit in `max_lines` ends in an
    // ellipsis. Words wider than `max_width` are broken between characters.
    [[nodiscard]] std::vector<std::string> wrap(std::string_view text, const TextStyle& style,
                                                float max_width, int max_lines) const;
    [[nodiscard]] std::string ellipsize(std::string_view text, const TextStyle& style,
                                        float max_width) const;

private:
    struct Shaped {
        char32_t code_point;
        std::uint16_t glyph;
        float pen;  // pen position relative to the start of the run
    };

    [[nodiscard]] const Font* font_for(FontWeight weight) const noexcept;
    [[nodiscard]] float legacy_scale(const TextStyle& style) const noexcept;
    [[nodiscard]] const GlyphImage& glyph_image(const Shaped& item, const TextStyle& style,
                                                int subpixel) const;
    // Returns the run's total advance and, when `output` is set, every drawable glyph.
    [[nodiscard]] float shape(std::string_view text, const TextStyle& style,
                              std::vector<Shaped>* output) const;
    [[nodiscard]] std::string fit_with_ellipsis(std::string text, const TextStyle& style,
                                                float max_width) const;

    std::unique_ptr<Font> regular_;
    std::unique_ptr<Font> semi_bold_;
    mutable std::unordered_map<std::uint64_t, GlyphImage> cache_;
};

}  // namespace frame_notify::ui
