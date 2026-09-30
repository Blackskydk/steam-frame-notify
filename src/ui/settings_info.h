#pragma once

#include <string>

namespace frame_notify::ui {

// What the settings screen shows and lets the user change.
struct SettingsInfo {
    bool autostart_enabled = false;   // Frame Notify starts by itself when the Frame starts
    std::string autostart_method;     // "systemd" or "desktop" when enabled, otherwise empty
    std::string message;              // the last problem, for example why autostart could not change
    std::string version;
    // The update check. `update_state` is empty until it has been asked for, then "checking",
    // "current", "available", "unknown" (the newest release cannot be compared) or "failed".
    std::string update_state;
    std::string update_latest;        // the newest release's version
    std::string update_message;       // why the check failed
};

}  // namespace frame_notify::ui
