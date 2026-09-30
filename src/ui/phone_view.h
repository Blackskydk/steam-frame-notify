#pragma once

#include "ui/phone_info.h"
#include "ui/renderer.h"
#include "ui/settings_info.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace frame_notify::ui {

enum class PhoneButton {
    kStartPairing,     // begin (or restart) pairing a phone
    kCancel,           // stop pairing
    kConfirm,          // "yes, the codes match"
    kReject,           // "no, they differ"
    kRemoveConflict,   // remove an already paired phone; the button's argument is its address
    kContinueAnyway,   // carry on pairing without removing anything
    kDismiss,          // acknowledge a pairing result and go back to the notifications
    kClose,            // leave this screen
    kRetry,            // look again: restart the helper or check Bluetooth again
    kPowerOn,          // turn the Bluetooth adapter on
    kForget,           // ask whether to forget the phone
    kForgetConfirmed,  // yes, forget it
    kForgetCancelled,  // no, keep it
    kToggleAutostart,  // settings: start with the Frame, or stop doing that
};

enum class ButtonStyle {
    kPrimary,
    kSecondary,
    kDanger,
};

struct PhoneScreenButton {
    PhoneButton id;
    std::string label;
    std::string argument;
    ButtonStyle style = ButtonStyle::kSecondary;
};

enum class PhoneIcon {
    kPhone,
    kBluetooth,
    kCheck,
    kWarning,
    kGear,
};

// Everything one screen says, before it is laid out: which step of pairing it is, what the user
// can press, and what text goes with it.
struct PhoneScreen {
    std::string title;
    std::vector<std::string> body;   // paragraphs
    std::vector<std::string> steps;  // numbered instructions
    std::string code;                // the pairing code, shown large
    std::string footer;              // a small line above the buttons, such as a countdown
    Tone tone = Tone::kNeutral;
    PhoneIcon icon = PhoneIcon::kPhone;
    std::vector<PhoneScreenButton> buttons;
};

// The screen for what the helper reports. `confirm_forget` replaces it with the "forget this
// phone?" question.
[[nodiscard]] PhoneScreen describe_phone_screen(const PhoneInfo& info, bool confirm_forget);
// The settings screen.
[[nodiscard]] PhoneScreen describe_settings_screen(const SettingsInfo& info);
// Whether "Forget this phone" makes sense in this state.
[[nodiscard]] bool phone_state_can_forget(const std::string& state);
// "4:32" for 272 seconds; negative counts as zero.
[[nodiscard]] std::string format_countdown(int seconds);

struct PhoneHit {
    PhoneButton button;
    std::string argument;
};

struct PhoneButtonRect {
    int left;
    int top;
    int right;
    int bottom;
};

// Lays out and paints one phone screen, and finds the button under the pointer. The card hangs from
// the top of the panel; the image is taller than the panel (by extra room below the card) so that
// the screen can be dragged up like the notification list.
class PhoneView {
public:
    void set(const PhoneInfo& info, bool confirm_forget);
    // Any card screen, such as the settings.
    void set_screen(PhoneScreen screen);

    [[nodiscard]] const PhoneScreen& screen() const noexcept { return screen_; }
    [[nodiscard]] std::uint64_t signature() const noexcept { return signature_; }
    [[nodiscard]] const std::vector<PhoneButtonRect>& button_rects() const noexcept {
        return button_rects_;
    }
    // Full image height: at least the panel's height plus some room to scroll.
    [[nodiscard]] int content_height() const noexcept { return content_height_; }
    [[nodiscard]] int maximum_scroll_offset() const noexcept;
    // `y` is a panel row; `scroll_offset` is how far the image is scrolled.
    [[nodiscard]] std::optional<PhoneHit> hit_test(int x, int y, int scroll_offset = 0) const;
    [[nodiscard]] std::vector<std::uint8_t> render() const;

private:
    struct TextLine {
        std::string text;
        float x;
        float baseline;
        TextStyle style;
        Color color;
        TextAlign align;
    };
    struct StepBadge {
        float x;
        float y;
        int number;
    };

    PhoneScreen screen_;
    std::uint64_t signature_ = 0;
    int content_height_ = 1024;
    std::vector<PhoneButtonRect> button_rects_;
    std::vector<TextLine> lines_;
    std::vector<StepBadge> badges_;
    PhoneButtonRect card_{};
    PhoneButtonRect code_box_{};
    float icon_x_ = 0.0F;
    float icon_y_ = 0.0F;
};

}  // namespace frame_notify::ui
