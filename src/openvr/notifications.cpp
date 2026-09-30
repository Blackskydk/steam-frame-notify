#include "openvr/notifications.h"

#include <openvr.h>

#include <iostream>
#include <string>

namespace frame_notify::openvr {
namespace {

constexpr std::uint64_t kTestUserValue = 1;
constexpr char kTestText[] = "Phone\nJane Doe: Hello from Frame Notify";

std::string bounded_notification_text(std::string_view text) {
    constexpr std::size_t kMaximumBytes = vr::k_unNotificationTextMaxSize - 1U;
    std::size_t offset = 0;
    std::size_t safe_end = 0;
    while (offset < text.size() && offset < kMaximumBytes) {
        const unsigned char lead = static_cast<unsigned char>(text[offset]);
        std::size_t sequence_size = 1;
        if ((lead & 0xE0U) == 0xC0U) sequence_size = 2;
        else if ((lead & 0xF0U) == 0xE0U) sequence_size = 3;
        else if ((lead & 0xF8U) == 0xF0U) sequence_size = 4;
        if (offset + sequence_size > text.size() || offset + sequence_size > kMaximumBytes) break;
        safe_end = offset + sequence_size;
        offset += sequence_size;
    }
    return std::string(text.substr(0, safe_end));
}

const char* notification_error_name(vr::EVRNotificationError error) {
    switch (error) {
    case vr::VRNotificationError_OK:
        return "VRNotificationError_OK";
    case vr::VRNotificationError_InvalidNotificationId:
        return "VRNotificationError_InvalidNotificationId";
    case vr::VRNotificationError_NotificationQueueFull:
        return "VRNotificationError_NotificationQueueFull";
    case vr::VRNotificationError_InvalidOverlayHandle:
        return "VRNotificationError_InvalidOverlayHandle";
    case vr::VRNotificationError_SystemWithUserValueAlreadyExists:
        return "VRNotificationError_SystemWithUserValueAlreadyExists";
    case vr::VRNotificationError_ServiceUnavailable:
        return "VRNotificationError_ServiceUnavailable";
    default:
        return "VRNotificationError_Unknown";
    }
}

}  // namespace

bool NativeNotification::bind(vr::IVRNotifications* notifications_api,
                              std::uint64_t overlay_handle) {
    if (notifications_api == nullptr) {
        std::cerr << "[Notification] " << vr::IVRNotifications_Version
                  << " is unavailable from this runtime\n";
        return false;
    }
    if (overlay_handle == vr::k_ulOverlayHandleInvalid) {
        std::cerr << "[Notification] Cannot create notification: invalid overlay handle\n";
        return false;
    }

    notifications_api_ = notifications_api;
    overlay_handle_ = overlay_handle;
    return true;
}

bool NativeNotification::show_test() {
    return show(kTestUserValue, kTestText);
}

bool NativeNotification::show(std::uint64_t user_value, std::string_view text) {
    if (notifications_api_ == nullptr || overlay_handle_ == vr::k_ulOverlayHandleInvalid) {
        std::cerr << "[Notification] Notification service is not bound to an overlay\n";
        return false;
    }

    const std::string bounded_text = bounded_notification_text(text);

    vr::VRNotificationId notification_id = 0;
    const auto error = notifications_api_->CreateNotification(
        overlay_handle_, user_value, vr::EVRNotificationType_Transient, bounded_text.c_str(),
        vr::EVRNotificationStyle_Application, nullptr, &notification_id);

    std::cout << "[Notification] CreateNotification returned: "
              << notification_error_name(error) << " (" << static_cast<int>(error) << ")\n"
              << "[Notification] id=" << notification_id << '\n';
    if (error != vr::VRNotificationError_OK) {
        return false;
    }

    std::cout << "[Notification] Type: Transient\n"
              << "[Notification] Style: Application\n"
              << "[Notification] Waiting for VR observation\n";
    return true;
}

}  // namespace frame_notify::openvr
