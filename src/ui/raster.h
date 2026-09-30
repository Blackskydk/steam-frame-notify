#pragma once

#include <vector>

namespace frame_notify::ui {

struct Point {
    float x;
    float y;
};

// A vector outline made of closed polylines. Curves are flattened as they are added, so the
// rasterizer only ever sees straight segments.
class Path {
public:
    void move_to(Point point);
    void line_to(Point point);
    void quad_to(Point control, Point point);
    void cubic_to(Point control1, Point control2, Point point);
    void close();

    // Whole closed shapes. A circle drawn with `reverse` set winds the other way, so putting one
    // inside another leaves a hole.
    void add_circle(Point center, float radius, bool reverse = false);
    // A thick line with round ends.
    void add_capsule(Point from, Point to, float radius);

    [[nodiscard]] bool empty() const noexcept { return contours_.empty(); }
    [[nodiscard]] const std::vector<std::vector<Point>>& contours() const noexcept {
        return contours_;
    }

private:
    void ensure_contour();

    std::vector<std::vector<Point>> contours_;
    Point current_{0.0F, 0.0F};
    Point start_{0.0F, 0.0F};
    bool open_ = false;
};

// Per-pixel coverage (0..1) of a path inside an integer pixel rectangle.
struct Coverage {
    int left = 0;
    int top = 0;
    int width = 0;
    int height = 0;
    std::vector<float> alpha;

    [[nodiscard]] bool empty() const noexcept { return width <= 0 || height <= 0; }
};

// Exact-area anti-aliased fill of every contour of `path` (overlaps count once), limited to the
// clip rectangle [clip_left, clip_right) x [clip_top, clip_bottom).
[[nodiscard]] Coverage rasterize(const Path& path, int clip_left, int clip_top, int clip_right,
                                 int clip_bottom);

}  // namespace frame_notify::ui
