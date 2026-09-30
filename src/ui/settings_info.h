#pragma once

#include <string>

namespace frame_notify::ui {

// What the settings screen shows and lets the user change.
struct SettingsInfo {
    bool autostart_enabled = false;   // Frame Notify starts by itself when the Frame starts
    std::string autostart_method;     // "systemd" or "desktop" when enabled, otherwise empty
    std::string message;              // the last problem, for example why autostart could not change
    std::string version;
};

}  // namespace frame_notify::ui
