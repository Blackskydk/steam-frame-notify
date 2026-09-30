#include "ui/scroll_texture.h"
#include "ui/theme.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <vector>

namespace {

using namespace frame_notify::ui;

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "scroll_texture_test.cpp:" << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(condition) expect((condition), #condition, __LINE__)

// A content image whose pixel at (x, row) says which row it is, so a copied row can be recognised.
std::vector<std::uint8_t> numbered_content(int rows) {
    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(rows) * ScrollTexture::kViewportWidth * 4U);
    for (int row = 0; row < rows; ++row) {
        for (int x = 0; x < ScrollTexture::kViewportWidth; ++x) {
            std::uint8_t* pixel = &pixels[(static_cast<std::size_t>(row) * ScrollTexture::kViewportWidth + x) * 4U];
            pixel[0] = static_cast<std::uint8_t>(row & 0xFF);
            pixel[1] = static_cast<std::uint8_t>((row >> 8) & 0xFF);
            pixel[2] = static_cast<std::uint8_t>(x & 0xFF);
            pixel[3] = 255;
        }
    }
    return pixels;
}

int row_number(const std::vector<std::uint8_t>& texture, int texture_row, int x = 0) {
    const std::uint8_t* pixel =
        &texture[(static_cast<std::size_t>(texture_row) * ScrollTexture::kWidth + x) * 4U];
    return pixel[0] | (pixel[1] << 8);
}

bool is_background(const std::vector<std::uint8_t>& texture, int texture_row, int x) {
    const std::uint8_t* pixel =
        &texture[(static_cast<std::size_t>(texture_row) * ScrollTexture::kWidth + x) * 4U];
    return pixel[0] == theme::kBackgroundBottom.red && pixel[1] == theme::kBackgroundBottom.green &&
           pixel[2] == theme::kBackgroundBottom.blue && pixel[3] == 255;
}

}  // namespace

int main() {
    // The texture has the viewport's shape: that is what keeps the clickable area equal to the picture.
    EXPECT(ScrollTexture::kWidth * ScrollTexture::kViewportHeight ==
           ScrollTexture::kHeight * ScrollTexture::kViewportWidth);
    EXPECT(ScrollTexture::kViewportWidth == 1280 && ScrollTexture::kViewportHeight == 800);

    // Placement: centred on the scroll position where the content allows, clamped at both ends.
    ScrollTexture texture;
    const int spare = ScrollTexture::kHeight - ScrollTexture::kViewportHeight;
    EXPECT(texture.place(0, 5000) == 0);
    EXPECT(texture.place(100, 5000) == 0);                          // too near the start to centre
    EXPECT(texture.place(2000, 5000) == 2000 - spare / 2);
    EXPECT(texture.place(4200, 5000) == 5000 - ScrollTexture::kHeight);   // the last rows, flush
    EXPECT(texture.place(-500, 5000) == 0);
    EXPECT(texture.place(9999, 5000) == 5000 - ScrollTexture::kHeight);
    EXPECT(texture.place(300, 900) == 0);                           // content shorter than the window
    EXPECT(texture.place(300, 0) == 0);
    // Scrolling down keeps most of the window ahead, scrolling up keeps it behind.
    EXPECT(texture.place(2000, 5000, 1) == 1900);
    EXPECT(texture.place(2000, 5000, -1) == 2000 - (spare - 100));
    EXPECT(texture.covers(1300) && texture.covers(2100) && !texture.covers(2101) && !texture.covers(1299));
    EXPECT(texture.place(2000, 5000, 0) == 2000 - spare / 2);
    EXPECT(texture.place(50, 5000, 1) == 0 && texture.place(4900, 5000, -1) == 5000 - ScrollTexture::kHeight);

    // Whether a scroll position is covered.
    texture.place(2000, 5000);
    const int top = texture.top();
    EXPECT(texture.covers(2000) && texture.covers(top) && texture.covers(top + spare));
    EXPECT(!texture.covers(top - 1) && !texture.covers(top + spare + 1));
    EXPECT(!texture.covers(0) && !texture.covers(4200));
    texture.place(0, 5000);
    EXPECT(texture.covers(0) && texture.covers(spare) && !texture.covers(spare + 1));

    // Bounds: the viewport's place in the texture, with the texture's own proportions.
    texture.place(2000, 5000);
    const TextureBounds at_centre = texture.bounds(2000);
    EXPECT(at_centre.u_min == 0.0F && at_centre.u_max == 0.5F);
    EXPECT(std::fabs(at_centre.v_min - 0.25F) < 1e-6F && std::fabs(at_centre.v_max - 0.75F) < 1e-6F);
    const TextureBounds at_top = texture.bounds(texture.top());
    EXPECT(at_top.v_min == 0.0F && std::fabs(at_top.v_max - 0.5F) < 1e-6F);
    const TextureBounds at_bottom = texture.bounds(texture.top() + spare);
    EXPECT(std::fabs(at_bottom.v_max - 1.0F) < 1e-6F);
    for (int offset = texture.top(); offset <= texture.top() + spare; offset += 37) {
        const TextureBounds bounds = texture.bounds(offset);
        // The shown part has the same proportions as the whole texture, and stays inside it.
        const float shown_width = (bounds.u_max - bounds.u_min) * ScrollTexture::kWidth;
        const float shown_height = (bounds.v_max - bounds.v_min) * ScrollTexture::kHeight;
        EXPECT(std::fabs(shown_width * ScrollTexture::kHeight - shown_height * ScrollTexture::kWidth) < 1.0F);
        EXPECT(bounds.v_min >= 0.0F && bounds.v_max <= 1.0F + 1e-6F && bounds.u_max <= 1.0F);
        // And it is the content row the caller asked for that sits at the top of the crop.
        EXPECT(std::lround(bounds.v_min * ScrollTexture::kHeight) + texture.top() == offset);
    }

    // Pointer positions. SteamVR reports a point of the whole texture, bottom-left origin, times the
    // mouse scale (set to the texture's size); panel_point() must undo exactly that, wherever the
    // crop is. The emulation below is what the Steam Frame reported in its logs: pointing at the
    // header's status chip (panel 950,78) gave half of each coordinate while the mouse scale was
    // half the texture's size.
    {
        ScrollTexture view;
        for (const int window_top_scroll : {0, 100, 2000, 4200}) {
            view.place(window_top_scroll, 5000);
            for (int offset = view.top(); offset <= view.top() + spare; offset += 97) {
                const TextureBounds crop = view.bounds(offset);
                for (const float x : {0.0F, 77.5F, 950.0F, 1279.0F}) {
                    for (const float y : {0.0F, 78.0F, 400.5F, 799.0F}) {
                        const float u = crop.u_min + (x / 1280.0F) * (crop.u_max - crop.u_min);
                        const float v = crop.v_min + (y / 800.0F) * (crop.v_max - crop.v_min);
                        const float event_x = u * ScrollTexture::kWidth;
                        const float event_y = (1.0F - v) * ScrollTexture::kHeight;
                        const PanelPoint point = view.panel_point(event_x, event_y, offset);
                        EXPECT(std::fabs(point.x - x) < 0.05F && std::fabs(point.y - y) < 0.05F);
                    }
                }
            }
        }
        view.place(0, 5000);
        const PanelPoint corner = view.panel_point(0.0F, static_cast<float>(ScrollTexture::kHeight), 0);
        EXPECT(corner.x == 0.0F && corner.y == 0.0F);                        // top-left of the panel
        const PanelPoint bottom_right = view.panel_point(1280.0F, 800.0F, 0);
        EXPECT(bottom_right.x == 1280.0F && bottom_right.y == 800.0F);      // bottom-right of the panel
        const PanelPoint scrolled = view.panel_point(300.0F, 1600.0F - 650.0F, 500);
        EXPECT(scrolled.x == 300.0F && scrolled.y == 150.0F);               // texture row 650 is 150 into the view
    }

    // Building the pixels: rows from top() at the left, background elsewhere.
    const int content_rows = 4000;
    const auto content = numbered_content(content_rows);
    texture.place(2000, content_rows);
    auto pixels = texture.build(content, content_rows);
    EXPECT(pixels.size() == static_cast<std::size_t>(ScrollTexture::kWidth) * ScrollTexture::kHeight * 4U);
    EXPECT(row_number(pixels, 0) == texture.top());
    EXPECT(row_number(pixels, 799) == texture.top() + 799);
    EXPECT(row_number(pixels, ScrollTexture::kHeight - 1) == texture.top() + ScrollTexture::kHeight - 1);
    EXPECT(pixels[2] == 0 && pixels[(1279) * 4U + 2] == (1279 & 0xFF));   // columns are copied in order
    EXPECT(is_background(pixels, 10, 1280) && is_background(pixels, 10, ScrollTexture::kWidth - 1));
    EXPECT(is_background(pixels, ScrollTexture::kHeight - 1, 2000));
    // Whatever the viewport shows at a covered position is exactly the content at that position.
    for (const int offset : {texture.top(), 2000, texture.top() + spare}) {
        const TextureBounds bounds = texture.bounds(offset);
        const int first_row = static_cast<int>(std::lround(bounds.v_min * ScrollTexture::kHeight));
        EXPECT(row_number(pixels, first_row) == offset);
        EXPECT(row_number(pixels, first_row + 799) == offset + 799);
    }

    // The end of the content: rows past it are background, not garbage or a crash.
    texture.place(content_rows - 800, content_rows);
    pixels = texture.build(content, content_rows);
    EXPECT(row_number(pixels, 0) == texture.top());
    EXPECT(row_number(pixels, ScrollTexture::kHeight - 1) == content_rows - 1);   // flush with the end
    // Short content leaves the rest of the window as background.
    const auto short_content = numbered_content(900);
    texture.place(0, 900);
    pixels = texture.build(short_content, 900);
    EXPECT(row_number(pixels, 899) == 899);
    EXPECT(is_background(pixels, 900, 0) && is_background(pixels, ScrollTexture::kHeight - 1, 1279));
    // A content buffer that is shorter than it claims is never read past its end.
    texture.place(0, 5000);
    pixels = texture.build(short_content, 5000);
    EXPECT(row_number(pixels, 899) == 899 && is_background(pixels, 900, 0));
    // Empty content is all background.
    pixels = texture.build({}, 0);
    EXPECT(is_background(pixels, 0, 0) && is_background(pixels, ScrollTexture::kHeight - 1, ScrollTexture::kWidth - 1));

    // Walking a whole list never leaves the window uncovered once the window is moved when needed.
    ScrollTexture walker;
    walker.place(0, 8000, 1);
    int moves = 0;
    for (int offset = 0; offset <= 8000 - 800; offset += 13) {
        if (!walker.covers(offset)) {
            walker.place(offset, 8000, 1);
            ++moves;
        }
        EXPECT(walker.covers(offset));
        if (failures > 10) break;
    }
    EXPECT(moves >= 8 && moves <= 13);                               // about one upload per 700 rows
    for (int offset = 8000 - 800; offset >= 0; offset -= 13) {       // and back up again
        if (!walker.covers(offset)) {
            walker.place(offset, 8000, -1);
            ++moves;
        }
        EXPECT(walker.covers(offset));
        if (failures > 10) break;
    }
    EXPECT(moves >= 16 && moves <= 26);

    if (failures != 0) {
        std::cerr << failures << " expectation(s) failed\n";
        return 1;
    }
    return 0;
}
