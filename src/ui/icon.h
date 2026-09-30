#pragma once

#include "ui/renderer.h"

#include <cstdint>
#include <vector>

namespace frame_notify::ui {

inline constexpr std::uint32_t kIconBytesPerPixel = 4;

// The SteamVR dashboard tile: a rounded gradient square with a bell. A red badge with the number
// of unread notifications ("99+" beyond 99) appears in the corner when `unread_count` is above 0.
std::vector<std::uint8_t> make_notification_icon(std::uint32_t size, int unread_count = 0);

// Vector glyphs shared by the tile and the notification panel. `height` is the glyph's full
// height in pixels; everything is drawn centred on the given point.
void draw_bell(Canvas& canvas, float center_x, float center_y, float height, Color color);
// A cog with eight teeth and a hole in the middle; `diameter` is the tips' span.
void draw_gear(Canvas& canvas, float center_x, float center_y, float diameter, Color color);
void draw_cross(Canvas& canvas, float center_x, float center_y, float arm, float thickness,
                Color color);
void draw_chevron(Canvas& canvas, float center_x, float center_y, float half_width,
                  float thickness, bool pointing_up, Color color);

}  // namespace frame_notify::ui
