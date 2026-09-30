#pragma once

#include "ui/phone_info.h"
#include "ui/renderer.h"

namespace frame_notify::ui::theme {

inline constexpr Color kBackgroundTop{12, 15, 23, 255};
inline constexpr Color kBackgroundBottom{8, 10, 15, 255};
inline constexpr Color kGlowBlue{74, 112, 255, 255};
inline constexpr Color kGlowViolet{150, 90, 255, 255};
inline constexpr Color kAccentBlue{118, 160, 255, 255};
inline constexpr Color kPrimaryTop{112, 156, 255, 255};     // the blue-to-violet of the main actions
inline constexpr Color kPrimaryBottom{96, 72, 246, 255};
inline constexpr Color kTextStrong{244, 247, 253, 255};
inline constexpr Color kTextUnreadTitle{244, 247, 253, 255};
inline constexpr Color kTextReadTitle{196, 205, 224, 255};
inline constexpr Color kTextUnreadBody{183, 192, 212, 255};
inline constexpr Color kTextReadBody{132, 142, 165, 255};
inline constexpr Color kTextMuted{122, 133, 158, 255};
inline constexpr Color kTextFaint{84, 95, 120, 255};
inline constexpr Color kLine{26, 32, 47, 255};
inline constexpr Color kCardUnreadTop{29, 36, 54, 255};
inline constexpr Color kCardUnreadBottom{23, 29, 44, 255};
inline constexpr Color kCardReadTop{20, 25, 38, 255};
inline constexpr Color kCardReadBottom{17, 21, 32, 255};
inline constexpr Color kBorderUnread{48, 59, 88, 255};
inline constexpr Color kBorderRead{29, 36, 53, 255};
inline constexpr Color kButtonFill{27, 33, 49, 255};
inline constexpr Color kButtonBorder{45, 54, 79, 255};
inline constexpr Color kWhite{255, 255, 255, 255};
inline constexpr Color kGood{52, 199, 89, 255};
inline constexpr Color kWarning{255, 179, 64, 255};
inline constexpr Color kBad{255, 92, 108, 255};

[[nodiscard]] inline Color tone_color(Tone tone) noexcept {
    switch (tone) {
    case Tone::kActive: return kAccentBlue;
    case Tone::kGood: return kGood;
    case Tone::kWarning: return kWarning;
    case Tone::kBad: return kBad;
    case Tone::kNeutral: break;
    }
    return kTextMuted;
}

// The dark gradient with its two soft colour glows, which every panel is drawn on.
inline void paint_background(Canvas& canvas, float width, float height) {
    canvas.fill_vertical_gradient(0.0F, 0.0F, width, height, kBackgroundTop, kBackgroundBottom);
    canvas.fill_radial_glow(180.0F, -20.0F, 700.0F, faded(kGlowBlue, 0.20F));
    canvas.fill_radial_glow(1180.0F, 40.0F, 460.0F, faded(kGlowViolet, 0.11F));
}

}  // namespace frame_notify::ui::theme
