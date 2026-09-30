#include "ui/scroll_controller.h"

#include <algorithm>
#include <cmath>

namespace frame_notify::ui {
namespace {

// Pointer travel that turns a press into a drag. A hand squeezing a trigger in VR wobbles, so this
// is generous (about 2.8 cm on the 1.5 m wide panel); a smaller value would swallow taps.
constexpr float kDragThreshold = 24.0F;
constexpr float kMinimumVelocity = 30.0F;     // below this momentum stops (pixels per second)
constexpr float kMaximumVelocity = 9000.0F;
constexpr float kMomentumLeftPerSecond = 0.05F;  // share of the speed still left after a second
constexpr float kPausedSeconds = 0.08F;       // holding still this long before letting go: no fling
constexpr float kMaximumTick = 0.1F;
constexpr float kGlideStep = 0.24F;           // share of the remaining distance covered per tick...
constexpr float kGlideTickSeconds = 0.025F;   // ...of this length

}  // namespace

float ScrollController::clamped(float value) const noexcept {
    return std::clamp(value, 0.0F, maximum_);
}

void ScrollController::reset() noexcept {
    position_ = target_ = last_position_ = 0.0F;
    velocity_ = 0.0F;
    pressed_ = dragging_ = stopped_momentum_ = false;
}

void ScrollController::jump_to(float position) noexcept {
    position_ = target_ = last_position_ = clamped(position);
    velocity_ = 0.0F;
    pressed_ = dragging_ = stopped_momentum_ = false;
}

void ScrollController::set_maximum(float maximum) noexcept {
    maximum_ = std::max(0.0F, maximum);
    position_ = clamped(position_);
    target_ = clamped(target_);
    last_position_ = clamped(last_position_);
}

void ScrollController::scroll_by(float delta) noexcept {
    velocity_ = 0.0F;
    target_ = clamped(target_ + delta);
}

void ScrollController::press(float pointer_y) noexcept {
    pressed_ = true;
    dragging_ = false;
    stopped_momentum_ = std::fabs(velocity_) > kMinimumVelocity;
    velocity_ = 0.0F;
    target_ = position_;  // a press also ends a wheel glide
    press_y_ = pointer_y;
    press_position_ = position_;
    last_position_ = position_;
    idle_seconds_ = 0.0F;
}

void ScrollController::move(float pointer_y) noexcept {
    if (!pressed_) return;
    const float travelled = pointer_y - press_y_;
    if (!dragging_) {
        if (std::fabs(travelled) < kDragThreshold) return;
        // Anchor at the point where the drag begins, so the content does not jump by the threshold.
        dragging_ = true;
        press_y_ = pointer_y;
        press_position_ = position_;
        return;
    }
    // Dragging the content down (pointer y grows) reveals what is above it.
    position_ = target_ = clamped(press_position_ - travelled);
}

bool ScrollController::release() noexcept {
    const bool tap = pressed_ && !dragging_ && !stopped_momentum_;
    if (dragging_) {
        if (idle_seconds_ > kPausedSeconds) velocity_ = 0.0F;
        velocity_ = std::clamp(velocity_, -kMaximumVelocity, kMaximumVelocity);
    } else {
        velocity_ = 0.0F;
    }
    pressed_ = dragging_ = false;
    return tap;
}

void ScrollController::cancel() noexcept {
    if (pressed_) static_cast<void>(release());
}

void ScrollController::tick(float seconds) noexcept {
    const float dt = std::clamp(seconds, 0.0F, kMaximumTick);
    if (pressed_) {
        if (dragging_ && dt > 0.0F) {
            // Follow the speed of the drag, smoothed, so letting go continues the motion.
            const float moved = position_ - last_position_;
            velocity_ = 0.5F * velocity_ + 0.5F * (moved / dt);
            idle_seconds_ = std::fabs(moved) < 0.5F ? idle_seconds_ + dt : 0.0F;
            last_position_ = position_;
        }
        return;
    }

    if (std::fabs(velocity_) > kMinimumVelocity) {
        position_ = target_ = clamped(position_ + velocity_ * dt);
        velocity_ *= std::pow(kMomentumLeftPerSecond, dt);
        const bool at_limit = (position_ <= 0.0F && velocity_ < 0.0F) ||
                              (position_ >= maximum_ && velocity_ > 0.0F);
        if (at_limit || std::fabs(velocity_) <= kMinimumVelocity) velocity_ = 0.0F;
        return;
    }
    velocity_ = 0.0F;

    const float remaining = target_ - position_;
    if (std::fabs(remaining) > 0.25F) {
        position_ += remaining * (1.0F - std::pow(1.0F - kGlideStep, dt / kGlideTickSeconds));
    } else {
        position_ = target_;
    }
}

}  // namespace frame_notify::ui
