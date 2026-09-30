#pragma once

#include <string>

namespace vr {
class IVRNotifications;
class IVROverlay;
class IVRSystem;
}

namespace frame_notify::openvr {

// Whether SteamVR is running, found out without starting it.
enum class SteamVrState {
    kRunning,
    kNotRunning,
    kUnavailable,   // no OpenVR runtime is installed, or it would not answer; see the detail
};

class Runtime {
public:
    // Asks SteamVR whether it is up by connecting as a background application, which never starts
    // it and is over at once. Do this in a short-lived process of its own: an application that has
    // connected to SteamVR and disconnected again is not reliably able to connect a second time.
    [[nodiscard]] static SteamVrState probe(std::string& detail);

    Runtime() = default;
    ~Runtime();

    Runtime(const Runtime&) = delete;
    Runtime& operator=(const Runtime&) = delete;

    bool initialize();

    [[nodiscard]] vr::IVRSystem* system() const noexcept { return system_; }
    [[nodiscard]] vr::IVROverlay* overlay() const noexcept { return overlay_; }
    [[nodiscard]] vr::IVRNotifications* notifications() const noexcept { return notifications_; }

private:
    vr::IVRSystem* system_ = nullptr;
    vr::IVROverlay* overlay_ = nullptr;
    vr::IVRNotifications* notifications_ = nullptr;
    bool initialized_ = false;
};

}  // namespace frame_notify::openvr
