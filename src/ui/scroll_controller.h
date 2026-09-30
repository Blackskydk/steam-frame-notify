#pragma once

namespace frame_notify::ui {

// Scroll position for the notification panel: mouse-wheel glides, click-and-drag scrolling with
// momentum, and the tap-versus-drag decision. Positions are in panel pixels from the top of the
// content; `pointer_y` is the pointer row in panel pixels, growing downwards. It knows nothing
// about OpenVR, so it is driven from the dashboard's event loop and unit tested on its own.
class ScrollController {
public:
    // Jump back to the top and stop all motion.
    void reset() noexcept;
    // Jump to `position` (clamped) and stop all motion.
    void jump_to(float position) noexcept;
    // The furthest the content can scroll; current and target positions are clamped to it.
    void set_maximum(float maximum) noexcept;
    // Mouse wheel or thumbstick: glides towards `position + delta`.
    void scroll_by(float delta) noexcept;

    // The pointer button went down. A press also stops any momentum scrolling.
    void press(float pointer_y) noexcept;
    // The pointer moved. Once it has travelled a few pixels the press becomes a drag and the
    // content follows the pointer one to one.
    void move(float pointer_y) noexcept;
    // The pointer button went up. Returns true when it was a plain tap: no drag, and it did not
    // merely stop a moving list.
    [[nodiscard]] bool release() noexcept;
    // The pointer left the panel: a drag ends as if released; a tap is discarded.
    void cancel() noexcept;

    // Advances momentum and gliding by `seconds` (clamped to a sane frame time).
    void tick(float seconds) noexcept;

    [[nodiscard]] float position() const noexcept { return position_; }
    [[nodiscard]] float velocity() const noexcept { return velocity_; }
    [[nodiscard]] bool pressed() const noexcept { return pressed_; }
    [[nodiscard]] bool dragging() const noexcept { return dragging_; }

private:
    float clamped(float value) const noexcept;

    float maximum_ = 0.0F;
    float position_ = 0.0F;
    float target_ = 0.0F;      // where a wheel glide is heading
    float velocity_ = 0.0F;    // pixels per second; positive scrolls further down the content
    float press_y_ = 0.0F;
    float press_position_ = 0.0F;
    float last_position_ = 0.0F;
    float idle_seconds_ = 0.0F;
    bool pressed_ = false;
    bool dragging_ = false;
    bool stopped_momentum_ = false;
};

}  // namespace frame_notify::ui
