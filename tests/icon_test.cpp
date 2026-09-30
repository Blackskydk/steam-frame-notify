#include "ui/icon.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <utility>
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

    // The gear is a ring of teeth around a hole: ink all around, none at the very centre.
    {
        frame_notify::ui::Canvas canvas(64, 64, {0, 0, 0, 255});
        frame_notify::ui::draw_gear(canvas, 32.0F, 32.0F, 48.0F, {255, 255, 255, 255});
        const auto gear = canvas.take_pixels();
        const auto brightness = [&](int x, int y) {
            return static_cast<int>(gear[(static_cast<std::size_t>(y) * 64U + static_cast<std::size_t>(x)) * 4U]);
        };
        if (brightness(32, 32) > 30 || brightness(32 + 13, 32) < 200 || brightness(32, 32 - 13) < 200 ||
            brightness(2, 2) > 10) {
            std::cerr << "The gear glyph has the wrong shape\n";
            return 1;
        }
        // The teeth stick out past the body, at the angles of a compass and its diagonals.
        if (brightness(32 + 22, 32) < 100 || brightness(32 + 16, 32 + 16) < 100 || brightness(32 + 22, 32 + 9) > 60) {
            std::cerr << "The gear glyph has no teeth where expected\n";
            return 1;
        }
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

    // SteamVR shows the tile small in the dock, so the number has to be big: the badge is at least
    // 100 pixels tall on the 256 pixel tile (it was 70), its digits are solid white ink well beyond
    // what the old small digits had (about 350 to 560 pixels), and all of it stays on the canvas.
    const auto is_white = [](const std::vector<std::uint8_t>& pixels, std::size_t index) {
        return pixels[index] > 235 && pixels[index + 1] > 235 && pixels[index + 2] > 235 && pixels[index + 3] > 200;
    };
    for (const int count : {3, 8, 12, 88, 150}) {
        const auto badged = frame_notify::ui::make_notification_icon(size, count);
        std::size_t digit_ink = 0;
        int left = static_cast<int>(size);
        int right = -1;
        int top = static_cast<int>(size);
        int bottom = -1;
        for (std::size_t index = 0; index + 3 < badged.size(); index += 4) {
            if (is_white(badged, index) && !is_white(none, index)) ++digit_ink;
            if (badged[index] > 220 && badged[index + 1] < 120 && badged[index + 2] < 140 && badged[index + 3] > 200) {
                const int x = static_cast<int>((index / 4) % size);
                const int y = static_cast<int>((index / 4) / size);
                left = std::min(left, x);
                right = std::max(right, x);
                top = std::min(top, y);
                bottom = std::max(bottom, y);
            }
        }
        if (bottom - top + 1 < 100 || digit_ink < 1000) {
            std::cerr << "The badge for " << count << " is too small to read in the dock (height "
                      << bottom - top + 1 << ", digit ink " << digit_ink << ")\n";
            return 1;
        }
        if (top < 4 || right > 250 || left < 40) {
            std::cerr << "The badge for " << count << " is cut off or covers too much of the tile (x " << left
                      << ".." << right << ", y " << top << ".." << bottom << ")\n";
            return 1;
        }
    }
    {   // Longer numbers make a wider badge, growing to the left from the same corner.
        const auto width_of = [&](int count) {
            const auto badged = frame_notify::ui::make_notification_icon(size, count);
            int left = static_cast<int>(size);
            int right = -1;
            for (std::size_t index = 0; index + 3 < badged.size(); index += 4) {
                if (badged[index] > 220 && badged[index + 1] < 120 && badged[index + 2] < 140 && badged[index + 3] > 200) {
                    left = std::min(left, static_cast<int>((index / 4) % size));
                    right = std::max(right, static_cast<int>((index / 4) % size));
                }
            }
            return std::pair<int, int>{left, right};
        };
        const auto one = width_of(1);
        const auto two = width_of(12);
        const auto three_characters = width_of(150);
        if (two.first >= one.first || three_characters.first >= two.first || one.second != two.second ||
            two.second != three_characters.second) {
            std::cerr << "The badge must grow to the left and keep its right edge\n";
            return 1;
        }
    }

    try {
        static_cast<void>(frame_notify::ui::make_notification_icon(16));
        std::cerr << "Small icon size should have been rejected\n";
        return 1;
    } catch (const std::invalid_argument&) {
    }

    return 0;
}

