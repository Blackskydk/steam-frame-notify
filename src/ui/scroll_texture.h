#pragma once

#include "ui/history_view.h"

#include <cstdint>
#include <vector>

namespace frame_notify::ui {

struct TextureBounds {
    float u_min;
    float v_min;
    float u_max;
    float v_max;
};

struct PanelPoint {
    float x;   // pixels from the panel's left edge
    float y;   // pixels from the top of what is shown
};

// The picture the overlay texture holds for a panel that scrolls by cropping.
//
// SteamVR sizes a dashboard panel, and the area that answers to the pointer, from the shape of the
// whole texture, while the texture bounds only choose what is drawn in it. A texture of another
// shape than the viewport therefore gives a panel whose clickable area is taller than what is
// drawn: it reaches over the SteamVR controls beneath and reports pointer positions that do not
// match the picture. So the texture is padded to exactly the viewport's shape (16:10), and is a
// window onto the content, a few screens tall, that is moved (by uploading it again) only when a
// scroll would leave it. Scrolling inside the window only changes the bounds.
//
// Pointer events report where the pointer is in the whole texture (its bottom-left origin, scaled
// by the overlay's mouse scale), not in the part that is shown. The mouse scale is therefore set to
// the texture's size in pixels, which makes an event's position a pixel of the texture, and
// panel_point() turns it into a position on the panel.
class ScrollTexture {
public:
    static constexpr int kViewportWidth = static_cast<int>(kHistoryViewWidth);
    static constexpr int kViewportHeight = static_cast<int>(kHistoryViewHeight);
    static constexpr int kWidth = 2560;    // content on the left, background padding on the right
    static constexpr int kHeight = 1600;   // rows of content held at a time
    static constexpr int kBytesPerPixel = 4;
    static_assert(kWidth * kViewportHeight == kHeight * kViewportWidth,
                  "the texture must have the viewport's aspect ratio");
    static_assert(kHeight >= kViewportHeight && kWidth >= kViewportWidth, "the window must fit the viewport");

    // Chooses the rows the texture will hold around `scroll_offset`, as far as the content allows.
    // With `direction` 0 the viewport sits in the middle; while scrolling down (positive) or up
    // (negative) most of the window lies ahead, so it lasts longer before it has to be moved.
    // Returns the content row at the texture's top.
    int place(int scroll_offset, int content_rows, int direction = 0) noexcept;
    // The content row at the texture's top, as last placed.
    [[nodiscard]] int top() const noexcept { return top_; }
    // Whether the placed window shows the whole viewport at `scroll_offset`.
    [[nodiscard]] bool covers(int scroll_offset) const noexcept;
    // Where in the texture the viewport sits at `scroll_offset` (which must be covered).
    [[nodiscard]] TextureBounds bounds(int scroll_offset) const noexcept;
    // A pointer position as reported by SteamVR (mouse scale = kWidth x kHeight, origin at the bottom
    // left) as a position on the panel, while the crop is at `scroll_offset`.
    [[nodiscard]] PanelPoint panel_point(float mouse_x, float mouse_y, int scroll_offset) const noexcept;
    // The texture's pixels: `content` (kViewportWidth wide, `content_rows` tall) from top() down, at
    // the left; the panel's background everywhere else.
    [[nodiscard]] std::vector<std::uint8_t> build(const std::vector<std::uint8_t>& content,
                                                  int content_rows) const;

private:
    int top_ = 0;
};

}  // namespace frame_notify::ui
