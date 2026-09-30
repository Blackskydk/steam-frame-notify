#pragma once

#include "ui/raster.h"
#include "ui/typography.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace frame_notify::ui {

struct Color {
    std::uint8_t red;
    std::uint8_t green;
    std::uint8_t blue;
    std::uint8_t alpha;
};

[[nodiscard]] Color mix(Color from, Color to, float amount) noexcept;
// Scales the alpha channel by `factor` (0..1).
[[nodiscard]] Color faded(Color color, float factor) noexcept;

enum class TextAlign {
    kLeft,
    kCenter,
    kRight,
};

// A straight-alpha RGBA surface with anti-aliased shape, gradient, shadow and text drawing.
// All coordinates are in pixels; shapes are sampled at pixel centres.
class Canvas {
public:
    Canvas(std::uint32_t width, std::uint32_t height, Color background);

    void fill_rect(float left, float top, float right, float bottom, Color color);
    void fill_rounded_rect(float left, float top, float right, float bottom, float radius,
                           Color color);
    // Vertical gradient from `top_color` at the top edge to `bottom_color` at the bottom edge.
    void fill_rounded_rect(float left, float top, float right, float bottom, float radius,
                           Color top_color, Color bottom_color);
    // A border of `thickness` drawn inside the given bounds.
    void stroke_rounded_rect(float left, float top, float right, float bottom, float radius,
                             float thickness, Color color);
    void fill_circle(float center_x, float center_y, float radius, Color color);
    void fill_vertical_gradient(float left, float top, float right, float bottom, Color top_color,
                                Color bottom_color);
    // Soft round highlight that fades to nothing at `radius`.
    void fill_radial_glow(float center_x, float center_y, float radius, Color color);
    // Soft drop shadow of a rounded rectangle; `blur` is the fade distance in pixels. The area
    // under the shape itself is left untouched, so paint the (opaque) shape afterwards.
    void draw_shadow(float left, float top, float right, float bottom, float radius, float blur,
                     float offset_y, Color color);
    void fill_path(const Path& path, Color color);
    // Draws one line of text with its baseline at `baseline` and returns its advance width. For
    // right and centre alignment `x` is the right edge and the centre respectively.
    float draw_text(float x, float baseline, std::string_view text, const TextStyle& style,
                    Color color, TextAlign align = TextAlign::kLeft);

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] const std::vector<std::uint8_t>& pixels() const noexcept { return pixels_; }
    [[nodiscard]] std::vector<std::uint8_t> take_pixels() noexcept;

private:
    void blend(int x, int y, Color color, float coverage);
    // Source-over with a floating-point source colour and alpha; the result is dithered.
    void blend_float(int x, int y, float red, float green, float blue, float alpha);

    std::uint32_t width_;
    std::uint32_t height_;
    std::vector<std::uint8_t> pixels_;
};

}  // namespace frame_notify::ui
