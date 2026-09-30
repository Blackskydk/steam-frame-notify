#include "ui/icon.h"

#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <string>

namespace frame_notify::ui {

void draw_bell(Canvas& canvas, float center_x, float center_y, float height, Color color) {
    // Designed on a 100 x 100 grid centred on the origin (y grows downwards).
    const float scale = height / 100.0F;
    const auto at = [&](float x, float y) {
        return Point{center_x + x * scale, center_y + y * scale};
    };

    Path body;
    body.move_to(at(-43, 24));
    body.cubic_to(at(-31, 18), at(-28, 6), at(-27, -12));
    body.cubic_to(at(-26, -28), at(-14, -37), at(0, -37));
    body.cubic_to(at(14, -37), at(26, -28), at(27, -12));
    body.cubic_to(at(28, 6), at(31, 18), at(43, 24));
    body.close();
    canvas.fill_path(body, color);

    canvas.fill_rounded_rect(center_x - 45.0F * scale, center_y + 20.0F * scale,
                             center_x + 45.0F * scale, center_y + 30.0F * scale, 5.0F * scale, color);
    canvas.fill_circle(center_x, center_y - 41.0F * scale, 5.5F * scale, color);
    canvas.fill_circle(center_x, center_y + 40.0F * scale, 9.0F * scale, color);
}

void draw_gear(Canvas& canvas, float center_x, float center_y, float diameter, Color color) {
    const float radius = diameter / 2.0F;
    constexpr int kTeeth = 8;
    constexpr float kTwoPi = 6.2831853F;
    Path path;
    path.add_circle({center_x, center_y}, radius * 0.66F);
    for (int tooth = 0; tooth < kTeeth; ++tooth) {
        const float angle = kTwoPi * static_cast<float>(tooth) / static_cast<float>(kTeeth);
        const float dx = std::cos(angle);
        const float dy = std::sin(angle);
        path.add_capsule({center_x + dx * radius * 0.62F, center_y + dy * radius * 0.62F},
                         {center_x + dx * radius * 0.83F, center_y + dy * radius * 0.83F}, radius * 0.17F);
    }
    path.add_circle({center_x, center_y}, radius * 0.30F, true);   // the hole
    canvas.fill_path(path, color);
}

void draw_cross(Canvas& canvas, float center_x, float center_y, float arm, float thickness,
                Color color) {
    Path path;
    path.add_capsule({center_x - arm, center_y - arm}, {center_x + arm, center_y + arm},
                     thickness * 0.5F);
    path.add_capsule({center_x + arm, center_y - arm}, {center_x - arm, center_y + arm},
                     thickness * 0.5F);
    canvas.fill_path(path, color);
}

void draw_chevron(Canvas& canvas, float center_x, float center_y, float half_width,
                  float thickness, bool pointing_up, Color color) {
    const float rise = half_width * 0.55F;
    const float tip_y = center_y + (pointing_up ? -rise : rise) * 0.5F;
    const float arm_y = center_y - (pointing_up ? -rise : rise) * 0.5F;
    Path path;
    path.add_capsule({center_x - half_width, arm_y}, {center_x, tip_y}, thickness * 0.5F);
    path.add_capsule({center_x, tip_y}, {center_x + half_width, arm_y}, thickness * 0.5F);
    canvas.fill_path(path, color);
}

std::vector<std::uint8_t> make_notification_icon(std::uint32_t size, int unread_count) {
    if (size < 32) {
        throw std::invalid_argument("notification icon must be at least 32 pixels wide");
    }

    Canvas canvas(size, size, {0, 0, 0, 0});
    const float scale = static_cast<float>(size) / 256.0F;
    const float left = 26.0F * scale;
    const float top = 22.0F * scale;
    const float right = 230.0F * scale;
    const float bottom = 226.0F * scale;
    const float radius = 58.0F * scale;

    canvas.draw_shadow(left, top, right, bottom, radius, 22.0F * scale, 10.0F * scale, {0, 0, 0, 120});
    canvas.fill_rounded_rect(left, top, right, bottom, radius, {112, 156, 255, 255}, {96, 72, 246, 255});
    // Soft highlight along the top edge for depth.
    canvas.fill_rounded_rect(left + 3.0F * scale, top + 3.0F * scale, right - 3.0F * scale,
                             top + 96.0F * scale, radius - 3.0F * scale, {255, 255, 255, 46},
                             {255, 255, 255, 0});

    draw_bell(canvas, 124.0F * scale, 132.0F * scale, 118.0F * scale, {255, 255, 255, 255});

    if (unread_count > 0) {
        // Unread badge with a ring that separates it from the tile.
        canvas.fill_circle(190.0F * scale, 74.0F * scale, 42.0F * scale, {84, 64, 226, 255});
        canvas.fill_circle(190.0F * scale, 74.0F * scale, 35.0F * scale, {255, 82, 102, 255});
        const std::string label = unread_count > 99 ? "99+" : std::to_string(unread_count);
        const TextStyle style{(label.size() == 1U ? 46.0F : label.size() == 2U ? 38.0F : 29.0F) * scale,
                              FontWeight::kSemiBold, 0.0F};
        canvas.draw_text(190.0F * scale,
                         74.0F * scale + Typography::shared().cap_height(style) / 2.0F, label,
                         style, {255, 255, 255, 255}, TextAlign::kCenter);
    }
    return canvas.take_pixels();
}

}  // namespace frame_notify::ui
