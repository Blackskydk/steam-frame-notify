#include "ui/renderer.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace frame_notify::ui {
namespace {

std::uint8_t lerp_channel(std::uint8_t from, std::uint8_t to, float amount) noexcept {
    const float value = static_cast<float>(from) + (static_cast<float>(to) - static_cast<float>(from)) * amount;
    return static_cast<std::uint8_t>(std::clamp(value + 0.5F, 0.0F, 255.0F));
}

// Ordered dither in (-0.5, 0.5): breaks up the banding that 8-bit output would otherwise show in
// slow gradients such as the dark background. Values that are exactly representable stay exact.
float dither_at(int x, int y) noexcept {
    static constexpr int kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
    return (static_cast<float>(kBayer[(y & 3) * 4 + (x & 3)]) + 0.5F) * (1.0F / 16.0F) - 0.5F;
}

std::uint8_t quantize(float value, float dither) noexcept {
    return static_cast<std::uint8_t>(std::clamp(std::floor(value + 0.5F + dither), 0.0F, 255.0F));
}

float smoothstep(float low, float high, float value) noexcept {
    const float t = std::clamp((value - low) / (high - low), 0.0F, 1.0F);
    return t * t * (3.0F - 2.0F * t);
}

// Signed distance from a point to a rounded box (negative inside).
float rounded_box_distance(float x, float y, float center_x, float center_y, float half_width,
                           float half_height, float radius) noexcept {
    const float qx = std::fabs(x - center_x) - (half_width - radius);
    const float qy = std::fabs(y - center_y) - (half_height - radius);
    const float outside_x = std::max(qx, 0.0F);
    const float outside_y = std::max(qy, 0.0F);
    const float outside = (outside_x > 0.0F || outside_y > 0.0F)
                              ? std::sqrt(outside_x * outside_x + outside_y * outside_y)
                              : 0.0F;
    return outside + std::min(std::max(qx, qy), 0.0F) - radius;
}

}  // namespace

Color mix(Color from, Color to, float amount) noexcept {
    amount = std::clamp(amount, 0.0F, 1.0F);
    return {lerp_channel(from.red, to.red, amount), lerp_channel(from.green, to.green, amount),
            lerp_channel(from.blue, to.blue, amount), lerp_channel(from.alpha, to.alpha, amount)};
}

Color faded(Color color, float factor) noexcept {
    color.alpha = static_cast<std::uint8_t>(
        std::clamp(static_cast<float>(color.alpha) * factor + 0.5F, 0.0F, 255.0F));
    return color;
}

Canvas::Canvas(std::uint32_t width, std::uint32_t height, Color background)
    : width_(width), height_(height), pixels_(static_cast<std::size_t>(width) * height * 4U) {
    for (std::size_t offset = 0; offset < pixels_.size(); offset += 4U) {
        pixels_[offset] = background.red;
        pixels_[offset + 1U] = background.green;
        pixels_[offset + 2U] = background.blue;
        pixels_[offset + 3U] = background.alpha;
    }
}

void Canvas::blend(int x, int y, Color color, float coverage) {
    blend_float(x, y, static_cast<float>(color.red), static_cast<float>(color.green),
                static_cast<float>(color.blue),
                static_cast<float>(color.alpha) * (1.0F / 255.0F) * coverage);
}

void Canvas::blend_float(int x, int y, float red, float green, float blue, float alpha) {
    if (x < 0 || y < 0 || x >= static_cast<int>(width_) || y >= static_cast<int>(height_)) return;
    if (alpha <= 0.002F) return;
    alpha = std::min(alpha, 1.0F);
    std::uint8_t* pixel =
        &pixels_[(static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(x)) * 4U];
    const float dither = dither_at(x, y);
    if (pixel[3] == 255U) {
        pixel[0] = quantize(static_cast<float>(pixel[0]) + (red - static_cast<float>(pixel[0])) * alpha, dither);
        pixel[1] = quantize(static_cast<float>(pixel[1]) + (green - static_cast<float>(pixel[1])) * alpha, dither);
        pixel[2] = quantize(static_cast<float>(pixel[2]) + (blue - static_cast<float>(pixel[2])) * alpha, dither);
        return;
    }
    // Straight-alpha "over" for translucent destinations (the app icon has a transparent border).
    const float destination_alpha = static_cast<float>(pixel[3]) * (1.0F / 255.0F);
    const float output_alpha = alpha + destination_alpha * (1.0F - alpha);
    const auto channel = [&](float source, std::uint8_t destination) {
        const float value = (source * alpha + static_cast<float>(destination) * destination_alpha *
                                                  (1.0F - alpha)) /
                            output_alpha;
        return quantize(value, dither);
    };
    pixel[0] = channel(red, pixel[0]);
    pixel[1] = channel(green, pixel[1]);
    pixel[2] = channel(blue, pixel[2]);
    pixel[3] = static_cast<std::uint8_t>(std::clamp(output_alpha * 255.0F + 0.5F, 0.0F, 255.0F));
}

void Canvas::fill_rect(float left, float top, float right, float bottom, Color color) {
    const int first_x = std::max(0, static_cast<int>(std::floor(left)));
    const int first_y = std::max(0, static_cast<int>(std::floor(top)));
    const int last_x = std::min(static_cast<int>(width_), static_cast<int>(std::ceil(right)));
    const int last_y = std::min(static_cast<int>(height_), static_cast<int>(std::ceil(bottom)));
    for (int y = first_y; y < last_y; ++y) {
        const float vertical = std::min(static_cast<float>(y + 1), bottom) -
                               std::max(static_cast<float>(y), top);
        for (int x = first_x; x < last_x; ++x) {
            const float horizontal = std::min(static_cast<float>(x + 1), right) -
                                     std::max(static_cast<float>(x), left);
            blend(x, y, color, std::clamp(horizontal, 0.0F, 1.0F) * std::clamp(vertical, 0.0F, 1.0F));
        }
    }
}

void Canvas::fill_rounded_rect(float left, float top, float right, float bottom, float radius,
                               Color color) {
    fill_rounded_rect(left, top, right, bottom, radius, color, color);
}

void Canvas::fill_rounded_rect(float left, float top, float right, float bottom, float radius,
                               Color top_color, Color bottom_color) {
    if (right <= left || bottom <= top) return;
    const float half_width = (right - left) * 0.5F;
    const float half_height = (bottom - top) * 0.5F;
    radius = std::clamp(radius, 0.0F, std::min(half_width, half_height));
    const float center_x = left + half_width;
    const float center_y = top + half_height;
    const int first_x = std::max(0, static_cast<int>(std::floor(left)) - 1);
    const int first_y = std::max(0, static_cast<int>(std::floor(top)) - 1);
    const int last_x = std::min(static_cast<int>(width_), static_cast<int>(std::ceil(right)) + 1);
    const int last_y = std::min(static_cast<int>(height_), static_cast<int>(std::ceil(bottom)) + 1);
    const bool gradient = top_color.red != bottom_color.red || top_color.green != bottom_color.green ||
                          top_color.blue != bottom_color.blue || top_color.alpha != bottom_color.alpha;

    for (int y = first_y; y < last_y; ++y) {
        const float sample_y = static_cast<float>(y) + 0.5F;
        const Color row_color =
            gradient ? mix(top_color, bottom_color, (sample_y - top) / (bottom - top)) : top_color;
        // In the straight band of the shape the middle of the row is fully covered.
        const bool straight_band = std::fabs(sample_y - center_y) <= half_height - radius - 0.5F;
        const bool has_span = straight_band && radius >= 1.0F;
        const int span_first =
            has_span ? std::max(first_x, static_cast<int>(std::ceil(left + radius))) : last_x;
        const int span_last =
            has_span ? std::min(last_x, static_cast<int>(std::floor(right - radius))) : last_x;
        const auto edge_pixel = [&](int x) {
            const float distance = rounded_box_distance(static_cast<float>(x) + 0.5F, sample_y,
                                                        center_x, center_y, half_width,
                                                        half_height, radius);
            const float coverage = std::clamp(0.5F - distance, 0.0F, 1.0F);
            if (coverage > 0.0F) blend(x, y, row_color, coverage);
        };
        for (int x = first_x; x < std::min(span_first, last_x); ++x) edge_pixel(x);
        if (span_last > span_first) {
            if (row_color.alpha == 255U) {
                std::uint8_t* pixel = &pixels_[(static_cast<std::size_t>(y) * width_ +
                                                static_cast<std::size_t>(span_first)) * 4U];
                for (int x = span_first; x < span_last; ++x, pixel += 4) {
                    pixel[0] = row_color.red;
                    pixel[1] = row_color.green;
                    pixel[2] = row_color.blue;
                    pixel[3] = 255U;
                }
            } else {
                for (int x = span_first; x < span_last; ++x) blend(x, y, row_color, 1.0F);
            }
        }
        for (int x = std::max(span_last, span_first); x < last_x; ++x) edge_pixel(x);
    }
}

void Canvas::stroke_rounded_rect(float left, float top, float right, float bottom, float radius,
                                 float thickness, Color color) {
    if (right <= left || bottom <= top || thickness <= 0.0F) return;
    const float half_width = (right - left) * 0.5F;
    const float half_height = (bottom - top) * 0.5F;
    radius = std::clamp(radius, 0.0F, std::min(half_width, half_height));
    const float center_x = left + half_width;
    const float center_y = top + half_height;
    const int first_x = std::max(0, static_cast<int>(std::floor(left)) - 1);
    const int first_y = std::max(0, static_cast<int>(std::floor(top)) - 1);
    const int last_x = std::min(static_cast<int>(width_), static_cast<int>(std::ceil(right)) + 1);
    const int last_y = std::min(static_cast<int>(height_), static_cast<int>(std::ceil(bottom)) + 1);
    for (int y = first_y; y < last_y; ++y) {
        const float sample_y = static_cast<float>(y) + 0.5F;
        // Only rows and columns within `thickness` of the border can be touched by the ring.
        const bool near_horizontal_edge = sample_y - top < thickness + 1.0F ||
                                          bottom - sample_y < thickness + 1.0F;
        for (int x = first_x; x < last_x; ++x) {
            const float sample_x = static_cast<float>(x) + 0.5F;
            const bool near_vertical_edge = sample_x - left < thickness + 1.0F ||
                                            right - sample_x < thickness + 1.0F;
            const bool in_corner = radius > 0.0F && (sample_x - left < radius + 1.0F ||
                                                     right - sample_x < radius + 1.0F) &&
                                   (sample_y - top < radius + 1.0F || bottom - sample_y < radius + 1.0F);
            if (!near_horizontal_edge && !near_vertical_edge && !in_corner) continue;
            const float distance = rounded_box_distance(sample_x, sample_y, center_x, center_y,
                                                        half_width, half_height, radius);
            const float outer = std::clamp(0.5F - distance, 0.0F, 1.0F);
            const float inner = std::clamp(0.5F - (distance + thickness), 0.0F, 1.0F);
            const float coverage = outer - inner;
            if (coverage > 0.0F) blend(x, y, color, coverage);
        }
    }
}

void Canvas::fill_circle(float center_x, float center_y, float radius, Color color) {
    fill_rounded_rect(center_x - radius, center_y - radius, center_x + radius, center_y + radius,
                      radius, color);
}

void Canvas::fill_vertical_gradient(float left, float top, float right, float bottom,
                                    Color top_color, Color bottom_color) {
    const int first_x = std::max(0, static_cast<int>(std::lround(left)));
    const int last_x = std::min(static_cast<int>(width_), static_cast<int>(std::lround(right)));
    const int first_y = std::max(0, static_cast<int>(std::lround(top)));
    const int last_y = std::min(static_cast<int>(height_), static_cast<int>(std::lround(bottom)));
    for (int y = first_y; y < last_y; ++y) {
        const float amount =
            std::clamp((static_cast<float>(y) + 0.5F - top) / std::max(1.0F, bottom - top), 0.0F, 1.0F);
        const auto channel = [amount](std::uint8_t from, std::uint8_t to) {
            return static_cast<float>(from) + (static_cast<float>(to) - static_cast<float>(from)) * amount;
        };
        const float red = channel(top_color.red, bottom_color.red);
        const float green = channel(top_color.green, bottom_color.green);
        const float blue = channel(top_color.blue, bottom_color.blue);
        const float alpha = channel(top_color.alpha, bottom_color.alpha) * (1.0F / 255.0F);
        if (alpha >= 0.999F) {
            // Opaque row: the dither only repeats every four columns, so quantise once per phase.
            std::uint8_t phase[4][3];
            for (int column = 0; column < 4; ++column) {
                const float dither = dither_at(column, y);
                phase[column][0] = quantize(red, dither);
                phase[column][1] = quantize(green, dither);
                phase[column][2] = quantize(blue, dither);
            }
            std::uint8_t* pixel =
                &pixels_[(static_cast<std::size_t>(y) * width_ + static_cast<std::size_t>(first_x)) * 4U];
            for (int x = first_x; x < last_x; ++x, pixel += 4) {
                const auto& value = phase[x & 3];
                pixel[0] = value[0];
                pixel[1] = value[1];
                pixel[2] = value[2];
                pixel[3] = 255U;
            }
            continue;
        }
        for (int x = first_x; x < last_x; ++x) blend_float(x, y, red, green, blue, alpha);
    }
}

void Canvas::fill_radial_glow(float center_x, float center_y, float radius, Color color) {
    if (radius <= 0.0F) return;
    const int first_x = std::max(0, static_cast<int>(std::floor(center_x - radius)));
    const int last_x = std::min(static_cast<int>(width_), static_cast<int>(std::ceil(center_x + radius)));
    const int first_y = std::max(0, static_cast<int>(std::floor(center_y - radius)));
    const int last_y = std::min(static_cast<int>(height_), static_cast<int>(std::ceil(center_y + radius)));
    for (int y = first_y; y < last_y; ++y) {
        const float dy = static_cast<float>(y) + 0.5F - center_y;
        for (int x = first_x; x < last_x; ++x) {
            const float dx = static_cast<float>(x) + 0.5F - center_x;
            const float falloff = 1.0F - smoothstep(0.0F, radius, std::sqrt(dx * dx + dy * dy));
            if (falloff > 0.0F) blend(x, y, color, falloff * falloff);
        }
    }
}

void Canvas::draw_shadow(float left, float top, float right, float bottom, float radius, float blur,
                         float offset_y, Color color) {
    if (right <= left || bottom <= top || blur <= 0.0F) return;
    const float caster_top = top;
    const float caster_bottom = bottom;
    top += offset_y;
    bottom += offset_y;
    const float half_width = (right - left) * 0.5F;
    const float half_height = (bottom - top) * 0.5F;
    radius = std::clamp(radius, 0.0F, std::min(half_width, half_height));
    const float center_x = left + half_width;
    const float center_y = top + half_height;
    const int first_x = std::max(0, static_cast<int>(std::floor(left - blur)));
    const int first_y = std::max(0, static_cast<int>(std::floor(std::min(top, caster_top) - blur)));
    const int last_x = std::min(static_cast<int>(width_), static_cast<int>(std::ceil(right + blur)));
    const int last_y = std::min(static_cast<int>(height_),
                                static_cast<int>(std::ceil(std::max(bottom, caster_bottom) + blur)));
    for (int y = first_y; y < last_y; ++y) {
        const float sample_y = static_cast<float>(y) + 0.5F;
        for (int x = first_x; x < last_x; ++x) {
            // The shape itself is painted over its shadow, so its opaque middle is skipped.
            const float sample_x = static_cast<float>(x) + 0.5F;
            if ((sample_x >= left + radius && sample_x <= right - radius && sample_y >= caster_top &&
                 sample_y <= caster_bottom) ||
                (sample_y >= caster_top + radius && sample_y <= caster_bottom - radius &&
                 sample_x >= left && sample_x <= right)) {
                continue;
            }
            const float distance = rounded_box_distance(sample_x, sample_y,
                                                        center_x, center_y, half_width,
                                                        half_height, radius);
            const float intensity = 1.0F - smoothstep(-blur * 0.5F, blur, distance);
            if (intensity > 0.0F) blend(x, y, color, intensity);
        }
    }
}

void Canvas::fill_path(const Path& path, Color color) {
    const Coverage coverage =
        rasterize(path, 0, 0, static_cast<int>(width_), static_cast<int>(height_));
    if (coverage.empty()) return;
    for (int row = 0; row < coverage.height; ++row) {
        for (int column = 0; column < coverage.width; ++column) {
            const float value = coverage.alpha[static_cast<std::size_t>(row) *
                                                   static_cast<std::size_t>(coverage.width) +
                                               static_cast<std::size_t>(column)];
            if (value > 0.0F) blend(coverage.left + column, coverage.top + row, color, value);
        }
    }
}

float Canvas::draw_text(float x, float baseline, std::string_view text, const TextStyle& style,
                        Color color, TextAlign align) {
    const Typography& typography = Typography::shared();
    const float width = typography.measure(text, style);
    if (align == TextAlign::kRight) x -= width;
    if (align == TextAlign::kCenter) x -= width * 0.5F;

    const int baseline_row = static_cast<int>(std::lround(baseline));
    for (const PlacedGlyph& glyph : typography.layout(text, style, x)) {
        const GlyphImage& image = *glyph.image;
        for (int row = 0; row < image.height; ++row) {
            for (int column = 0; column < image.width; ++column) {
                const std::uint8_t value = image.alpha[static_cast<std::size_t>(row) *
                                                           static_cast<std::size_t>(image.width) +
                                                       static_cast<std::size_t>(column)];
                if (value == 0U) continue;
                blend(glyph.x + image.left + column, baseline_row + image.top + row, color,
                      static_cast<float>(value) * (1.0F / 255.0F));
            }
        }
    }
    return width;
}

std::vector<std::uint8_t> Canvas::take_pixels() noexcept {
    return std::move(pixels_);
}

}  // namespace frame_notify::ui
