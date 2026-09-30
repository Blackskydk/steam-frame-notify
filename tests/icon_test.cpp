#include "ui/icon.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

int main() {
    constexpr std::uint32_t size = 256;
    const auto icon = frame_notify::ui::make_notification_icon(size);
    const auto expected_size = static_cast<std::size_t>(size) * size *
                               frame_notify::ui::kIconBytesPerPixel;
    if (icon.size() != expected_size) {
        std::cerr << "Unexpected icon buffer size\n";
        return 1;
    }

    std::size_t visible_pixels = 0;
    std::size_t opaque_pixels = 0;
    for (std::size_t index = 3; index < icon.size(); index += 4) {
        visible_pixels += icon[index] > 0 ? 1U : 0U;
        opaque_pixels += icon[index] == 255 ? 1U : 0U;
    }
    if (visible_pixels < 30'000 || opaque_pixels < 20'000) {
        std::cerr << "Icon is unexpectedly empty\n";
        return 1;
    }

    // The red badge appears only when something is unread, and it shows the count.
    const auto red_pixels = [](const std::vector<std::uint8_t>& pixels) {
        std::size_t count = 0;
        for (std::size_t index = 0; index + 3 < pixels.size(); index += 4) {
            if (pixels[index] > 220 && pixels[index + 1] < 120 && pixels[index + 2] < 140 &&
                pixels[index + 3] > 200) {
                ++count;
            }
        }
        return count;
    };
    const auto none = frame_notify::ui::make_notification_icon(size, 0);
    const auto three = frame_notify::ui::make_notification_icon(size, 3);
    const auto twelve = frame_notify::ui::make_notification_icon(size, 12);
    const auto many = frame_notify::ui::make_notification_icon(size, 5000);
    if (none != icon || frame_notify::ui::make_notification_icon(size, -4) != icon) {
        std::cerr << "Zero or negative unread counts must draw no badge\n";
        return 1;
    }
    if (red_pixels(none) > 100 || red_pixels(three) < 2000 || red_pixels(twelve) < 2000 ||
        red_pixels(many) < 2000) {
        std::cerr << "Unread badge is missing or has the wrong size\n";
        return 1;
    }
    if (three == twelve || twelve == many || three == none) {
        std::cerr << "Different unread counts must render differently\n";
        return 1;
    }

    try {
        static_cast<void>(frame_notify::ui::make_notification_icon(16));
        std::cerr << "Small icon size should have been rejected\n";
        return 1;
    } catch (const std::invalid_argument&) {
    }

    return 0;
}

