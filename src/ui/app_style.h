#pragma once

#include "ui/renderer.h"

#include <string>
#include <string_view>

namespace frame_notify::ui {

struct AppStyle {
    std::string name;      // friendly name shown on the card
    Color accent{0, 0, 0, 255};  // avatar and highlight colour
    std::string monogram;  // one letter (UTF-8) shown in the avatar
};

// `app` is what the sender supplied: a display name such as "Messages" or, from older senders, a
// bundle identifier such as "com.apple.MobileSMS". `app_id` is the bundle identifier when known.
// A readable `app` is always kept; identifiers are turned into names, and well-known apps get
// their usual colour while everything else gets a stable colour derived from its name.
[[nodiscard]] AppStyle resolve_app_style(std::string_view app, std::string_view app_id = {});

// True for reverse-DNS identifiers such as "io.heckel.ntfy".
[[nodiscard]] bool looks_like_bundle_id(std::string_view text) noexcept;

}  // namespace frame_notify::ui
