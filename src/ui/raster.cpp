#include "ui/raster.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <utility>

namespace frame_notify::ui {
namespace {

constexpr float kFlattenTolerance = 0.05F;
constexpr int kMaximumCurveSegments = 64;

int segment_count(float deviation) {
    const float count = std::ceil(std::sqrt(deviation / (4.0F * kFlattenTolerance)));
    if (!(count >= 1.0F)) return 1;
    return std::min(static_cast<int>(count), kMaximumCurveSegments);
}

// Accumulates signed area so that a running sum along each row yields exact coverage.
class Accumulator {
public:
    Accumulator(int width, int height)
        : width_(width), height_(height), stride_(static_cast<std::size_t>(width) + 2U),
          cells_(stride_ * static_cast<std::size_t>(height), 0.0F) {}

    // Splits the segment at the left and right edges so x stays inside [0, width]. Anything left of
    // the buffer still covers everything to its right, so it becomes a vertical edge at x = 0.
    void add_line(Point from, Point to) {
        const auto right = static_cast<float>(width_);
        if (from.x <= 0.0F && to.x <= 0.0F) {
            accumulate({0.0F, from.y}, {0.0F, to.y});
        } else if (from.x >= right && to.x >= right) {
            accumulate({right, from.y}, {right, to.y});
        } else if ((from.x < 0.0F) != (to.x < 0.0F)) {
            const float t = (0.0F - from.x) / (to.x - from.x);
            const Point middle{0.0F, from.y + t * (to.y - from.y)};
            add_line(from, middle);
            add_line(middle, to);
        } else if ((from.x > right) != (to.x > right)) {
            const float t = (right - from.x) / (to.x - from.x);
            const Point middle{right, from.y + t * (to.y - from.y)};
            add_line(from, middle);
            add_line(middle, to);
        } else {
            accumulate(from, to);
        }
    }

    void resolve(std::vector<float>& output) const {
        output.assign(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_), 0.0F);
        for (int y = 0; y < height_; ++y) {
            const float* row = &cells_[static_cast<std::size_t>(y) * stride_];
            float* destination = &output[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_)];
            float sum = 0.0F;
            for (int x = 0; x < width_; ++x) {
                sum += row[x];
                const float coverage = std::min(std::fabs(sum), 1.0F);
                destination[x] = coverage < 0.002F ? 0.0F : coverage;
            }
        }
    }

private:
    void accumulate(Point from, Point to) {
        if (from.y == to.y) return;
        float direction = 1.0F;
        if (from.y > to.y) {
            std::swap(from, to);
            direction = -1.0F;
        }
        if (to.y <= 0.0F || from.y >= static_cast<float>(height_)) return;

        const float slope = (to.x - from.x) / (to.y - from.y);
        const int first_row = std::max(0, static_cast<int>(std::floor(from.y)));
        const int last_row = std::min(height_, static_cast<int>(std::ceil(to.y)));
        for (int y = first_row; y < last_row; ++y) {
            const float top = std::max(from.y, static_cast<float>(y));
            const float bottom = std::min(to.y, static_cast<float>(y + 1));
            const float rise = bottom - top;
            if (rise <= 0.0F) continue;
            const float x_top = from.x + (top - from.y) * slope;
            const float x_bottom = from.x + (bottom - from.y) * slope;
            add_span(y, std::min(x_top, x_bottom), std::max(x_top, x_bottom), rise * direction);
        }
    }

    void add_span(int y, float left, float right, float signed_rise) {
        const auto limit = static_cast<float>(width_);
        left = std::clamp(left, 0.0F, limit);
        right = std::clamp(right, 0.0F, limit);
        float* row = &cells_[static_cast<std::size_t>(y) * stride_];
        const int first = static_cast<int>(std::floor(left));
        if (right - left < 1.0e-5F) {
            const float offset = left - static_cast<float>(first);
            row[first] += signed_rise * (1.0F - offset);
            row[first + 1] += signed_rise * offset;
            return;
        }
        const float span = right - left;
        const int last = std::max(first, static_cast<int>(std::ceil(right)) - 1);
        for (int column = first; column <= last; ++column) {
            const float from = std::max(left, static_cast<float>(column));
            const float to = std::min(right, static_cast<float>(column + 1));
            const float share = signed_rise * (to - from) / span;
            const float offset = 0.5F * (from + to) - static_cast<float>(column);
            row[column] += share * (1.0F - offset);
            row[column + 1] += share * offset;
        }
    }

    int width_;
    int height_;
    std::size_t stride_;
    std::vector<float> cells_;
};

}  // namespace

void Path::ensure_contour() {
    if (!open_) {
        contours_.push_back({current_});
        start_ = current_;
        open_ = true;
    }
}

void Path::move_to(Point point) {
    current_ = point;
    start_ = point;
    open_ = false;
}

void Path::line_to(Point point) {
    ensure_contour();
    contours_.back().push_back(point);
    current_ = point;
}

void Path::quad_to(Point control, Point point) {
    ensure_contour();
    const Point from = current_;
    const float deviation = std::hypot(from.x - 2.0F * control.x + point.x,
                                       from.y - 2.0F * control.y + point.y);
    const int steps = segment_count(deviation);
    for (int step = 1; step <= steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        const float u = 1.0F - t;
        contours_.back().push_back({u * u * from.x + 2.0F * u * t * control.x + t * t * point.x,
                                    u * u * from.y + 2.0F * u * t * control.y + t * t * point.y});
    }
    current_ = point;
}

void Path::cubic_to(Point control1, Point control2, Point point) {
    ensure_contour();
    const Point from = current_;
    const float deviation = std::max(
        std::hypot(from.x - 2.0F * control1.x + control2.x, from.y - 2.0F * control1.y + control2.y),
        std::hypot(control1.x - 2.0F * control2.x + point.x,
                   control1.y - 2.0F * control2.y + point.y));
    const int steps = segment_count(deviation * 0.75F);
    for (int step = 1; step <= steps; ++step) {
        const float t = static_cast<float>(step) / static_cast<float>(steps);
        const float u = 1.0F - t;
        const float a = u * u * u;
        const float b = 3.0F * u * u * t;
        const float c = 3.0F * u * t * t;
        const float d = t * t * t;
        contours_.back().push_back({a * from.x + b * control1.x + c * control2.x + d * point.x,
                                    a * from.y + b * control1.y + c * control2.y + d * point.y});
    }
    current_ = point;
}

void Path::close() {
    if (open_) {
        current_ = start_;
        open_ = false;
    }
}

void Path::add_circle(Point center, float radius, bool reverse) {
    constexpr int kSteps = 48;
    constexpr float kTwoPi = 6.28318530717959F;
    for (int step = 0; step < kSteps; ++step) {
        const float turn = static_cast<float>(step) / static_cast<float>(kSteps) * kTwoPi;
        const float theta = reverse ? -turn : turn;
        const Point point{center.x + radius * std::cos(theta), center.y + radius * std::sin(theta)};
        if (step == 0) move_to(point);
        else line_to(point);
    }
    close();
}

void Path::add_capsule(Point from, Point to, float radius) {
    constexpr int kArcSteps = 14;
    constexpr float kPi = 3.14159265358979F;
    const float angle = std::atan2(to.y - from.y, to.x - from.x);
    for (int side = 0; side < 2; ++side) {
        const Point center = side == 0 ? to : from;
        const float start = angle + (side == 0 ? -kPi / 2.0F : kPi / 2.0F);
        for (int step = 0; step <= kArcSteps; ++step) {
            const float theta = start + kPi * static_cast<float>(step) / static_cast<float>(kArcSteps);
            const Point point{center.x + radius * std::cos(theta), center.y + radius * std::sin(theta)};
            if (side == 0 && step == 0) move_to(point);
            else line_to(point);
        }
    }
    close();
}

Coverage rasterize(const Path& path, int clip_left, int clip_top, int clip_right,
                   int clip_bottom) {
    Coverage coverage;
    if (path.empty() || clip_right <= clip_left || clip_bottom <= clip_top) return coverage;

    float minimum_x = 1.0e30F;
    float minimum_y = 1.0e30F;
    float maximum_x = -1.0e30F;
    float maximum_y = -1.0e30F;
    for (const auto& contour : path.contours()) {
        for (const auto& point : contour) {
            minimum_x = std::min(minimum_x, point.x);
            minimum_y = std::min(minimum_y, point.y);
            maximum_x = std::max(maximum_x, point.x);
            maximum_y = std::max(maximum_y, point.y);
        }
    }
    if (minimum_x > maximum_x || minimum_y > maximum_y) return coverage;

    const int left = std::max(clip_left, static_cast<int>(std::floor(minimum_x)));
    const int top = std::max(clip_top, static_cast<int>(std::floor(minimum_y)));
    const int right = std::min(clip_right, static_cast<int>(std::ceil(maximum_x)));
    const int bottom = std::min(clip_bottom, static_cast<int>(std::ceil(maximum_y)));
    if (right <= left || bottom <= top) return coverage;

    coverage.left = left;
    coverage.top = top;
    coverage.width = right - left;
    coverage.height = bottom - top;

    Accumulator accumulator(coverage.width, coverage.height);
    const auto offset_x = static_cast<float>(left);
    const auto offset_y = static_cast<float>(top);
    for (const auto& contour : path.contours()) {
        if (contour.size() < 2U) continue;
        for (std::size_t index = 0; index < contour.size(); ++index) {
            const Point from = contour[index];
            const Point to = contour[(index + 1U) % contour.size()];
            accumulator.add_line({from.x - offset_x, from.y - offset_y},
                                 {to.x - offset_x, to.y - offset_y});
        }
    }
    accumulator.resolve(coverage.alpha);
    return coverage;
}

}  // namespace frame_notify::ui
