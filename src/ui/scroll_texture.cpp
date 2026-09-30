#include "ui/scroll_texture.h"

#include "ui/theme.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace frame_notify::ui {

int ScrollTexture::place(int scroll_offset, int content_rows, int direction) noexcept {
    constexpr int kBehind = 100;   // rows kept on the side the scroll came from
    const int spare = kHeight - kViewportHeight;
    const int before = direction > 0 ? kBehind : direction < 0 ? spare - kBehind : spare / 2;
    const int last_top = std::max(0, content_rows - kHeight);
    top_ = std::clamp(scroll_offset - before, 0, last_top);
    return top_;
}

bool ScrollTexture::covers(int scroll_offset) const noexcept {
    return scroll_offset >= top_ && scroll_offset + kViewportHeight <= top_ + kHeight;
}

TextureBounds ScrollTexture::bounds(int scroll_offset) const noexcept {
    const float first = static_cast<float>(scroll_offset - top_) / static_cast<float>(kHeight);
    return {0.0F, first, static_cast<float>(kViewportWidth) / static_cast<float>(kWidth),
            first + static_cast<float>(kViewportHeight) / static_cast<float>(kHeight)};
}

PanelPoint ScrollTexture::panel_point(float mouse_x, float mouse_y, int scroll_offset) const noexcept {
    const float texture_row = static_cast<float>(kHeight) - mouse_y;
    return {mouse_x, texture_row - static_cast<float>(scroll_offset - top_)};
}

std::vector<std::uint8_t> ScrollTexture::build(const std::vector<std::uint8_t>& content,
                                               int content_rows) const {
    const Color background = theme::kBackgroundBottom;
    const std::uint8_t pixel[kBytesPerPixel] = {background.red, background.green, background.blue, 255};
    std::vector<std::uint8_t> texture(
        static_cast<std::size_t>(kWidth) * kHeight * kBytesPerPixel);
    for (std::size_t offset = 0; offset < texture.size(); offset += kBytesPerPixel) {
        std::memcpy(texture.data() + offset, pixel, kBytesPerPixel);
    }

    const std::size_t content_row_bytes = static_cast<std::size_t>(kViewportWidth) * kBytesPerPixel;
    const std::size_t texture_row_bytes = static_cast<std::size_t>(kWidth) * kBytesPerPixel;
    const int available = std::min(content_rows, static_cast<int>(content.size() / content_row_bytes));
    const int rows = std::max(0, std::min(kHeight, available - top_));
    for (int row = 0; row < rows; ++row) {
        std::memcpy(texture.data() + static_cast<std::size_t>(row) * texture_row_bytes,
                    content.data() + static_cast<std::size_t>(top_ + row) * content_row_bytes,
                    content_row_bytes);
    }
    return texture;
}

}  // namespace frame_notify::ui
