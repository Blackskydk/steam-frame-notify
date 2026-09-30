#pragma once

namespace vr {
class IVRNotifications;
class IVROverlay;
class IVRSystem;
}

namespace frame_notify::openvr {

class Runtime {
public:
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
