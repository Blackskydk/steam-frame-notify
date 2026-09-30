#pragma once

#include "ui/raster.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace frame_notify::ui {

// Minimal TrueType (glyf) reader: character map, metrics, GPOS pair kerning and outlines.
// It reads only fonts it is given and never touches system fonts.
class Font {
public:
    // Returns nullptr when the file is missing or is not a usable TrueType font.
    [[nodiscard]] static std::unique_ptr<Font> load(const std::string& path);
    [[nodiscard]] static std::unique_ptr<Font> from_memory(std::vector<std::uint8_t> data);

    [[nodiscard]] int units_per_em() const noexcept { return units_per_em_; }
    [[nodiscard]] int ascender() const noexcept { return ascender_; }
    [[nodiscard]] int descender() const noexcept { return descender_; }
    [[nodiscard]] int line_gap() const noexcept { return line_gap_; }
    [[nodiscard]] int cap_height() const noexcept { return cap_height_; }
    [[nodiscard]] int x_height() const noexcept { return x_height_; }

    // Glyph 0 (.notdef) means the font has no glyph for the code point.
    [[nodiscard]] std::uint16_t glyph_index(char32_t code_point) const;
    [[nodiscard]] int advance(std::uint16_t glyph) const;
    // Horizontal adjustment in font units to apply between two adjacent glyphs.
    [[nodiscard]] int kerning(std::uint16_t left, std::uint16_t right) const;
    // Outline in font units with the y axis pointing up.
    [[nodiscard]] Path outline(std::uint16_t glyph) const;

private:
    struct Transform {
        float a = 1.0F;
        float b = 0.0F;
        float c = 0.0F;
        float d = 1.0F;
        float e = 0.0F;
        float f = 0.0F;
    };
    struct GlyphPoint {
        float x;
        float y;
        bool on_curve;
    };

    Font() = default;

    bool parse();
    void parse_kerning(std::size_t gpos);
    void append_glyph(std::uint16_t glyph, const Transform& transform, Path& path, int depth) const;
    bool pair_adjustment(std::size_t subtable, std::uint16_t left, std::uint16_t right,
                         int& value) const;
    [[nodiscard]] int coverage_index(std::size_t coverage, std::uint16_t glyph) const;
    [[nodiscard]] int glyph_class(std::size_t class_def, std::uint16_t glyph) const;

    [[nodiscard]] std::uint8_t u8(std::size_t offset) const noexcept {
        return offset < data_.size() ? data_[offset] : 0;
    }
    [[nodiscard]] std::uint16_t u16(std::size_t offset) const noexcept {
        return static_cast<std::uint16_t>((static_cast<unsigned>(u8(offset)) << 8U) |
                                          u8(offset + 1U));
    }
    [[nodiscard]] std::int16_t i16(std::size_t offset) const noexcept {
        return static_cast<std::int16_t>(u16(offset));
    }
    [[nodiscard]] std::uint32_t u32(std::size_t offset) const noexcept {
        return (static_cast<std::uint32_t>(u16(offset)) << 16U) | u16(offset + 2U);
    }

    std::vector<std::uint8_t> data_;
    int units_per_em_ = 1000;
    int ascender_ = 0;
    int descender_ = 0;
    int line_gap_ = 0;
    int cap_height_ = 0;
    int x_height_ = 0;
    int glyph_count_ = 0;
    int horizontal_metric_count_ = 0;
    bool long_loca_ = false;
    std::size_t hmtx_ = 0;
    std::size_t loca_ = 0;
    std::size_t glyf_ = 0;
    std::size_t glyf_length_ = 0;
    std::size_t cmap_subtable_ = 0;
    int cmap_format_ = 0;
    std::vector<std::vector<std::size_t>> kerning_lookups_;
    mutable std::unordered_map<std::uint32_t, int> kerning_cache_;
};

}  // namespace frame_notify::ui
