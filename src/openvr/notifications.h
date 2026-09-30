#pragma once

#include <cstdint>
#include <string_view>

namespace vr {
class IVRNotifications;
}

namespace frame_notify::openvr {

class NativeNotification {
public:
    NativeNotification() = default;
    ~NativeNotification() = default;

    NativeNotification(const NativeNotification&) = delete;
    NativeNotification& operator=(const NativeNotification&) = delete;

    bool bind(vr::IVRNotifications* notifications_api, std::uint64_t overlay_handle);
    bool show_test();
    bool show(std::uint64_t user_value, std::string_view text);

private:
    vr::IVRNotifications* notifications_api_ = nullptr;
    std::uint64_t overlay_handle_ = 0;
};

}  // namespace frame_notify::openvr
