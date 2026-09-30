#pragma once

#include <map>
#include <string>

namespace frame_notify::ui {

// What the Bluetooth helper last reported about the phone, see scripts/ancs_service.py for the
// states and fields. `state` is one of the helper's states, or "helper_unavailable" and
// "helper_restarting" when the helper process itself is not running.
struct PhoneInfo {
    std::string state = "starting";
    std::map<std::string, std::string> fields;
    int seconds_in_state = 0;  // how long the state has lasted, filled in by the dashboard

    [[nodiscard]] std::string field(const std::string& name) const;
};

enum class Tone {
    kNeutral,
    kActive,   // something is in progress or waiting for the user
    kGood,
    kWarning,
    kBad,
};

// The short status shown in the panel's header, such as "iPhone connected".
struct PhoneChip {
    std::string label;
    Tone tone = Tone::kNeutral;
};

[[nodiscard]] PhoneChip phone_chip(const PhoneInfo& info);
// Any state of the pairing flow.
[[nodiscard]] bool phone_state_is_pairing(const std::string& state);
// States in which the user has to do something, so the phone screen should come forward.
[[nodiscard]] bool phone_state_needs_attention(const std::string& state);

}  // namespace frame_notify::ui
