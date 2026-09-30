#include "openvr/runtime.h"

#include <openvr.h>

#include <array>
#include <cstdint>
#include <iostream>
#include <string>

namespace frame_notify::openvr {

Runtime::~Runtime() {
    if (initialized_) {
        vr::VR_Shutdown();
        std::cout << "[OpenVR] Runtime shut down\n";
    }
}

SteamVrState Runtime::probe(std::string& detail) {
    if (!vr::VR_IsRuntimeInstalled()) {
        detail = "No registered OpenVR runtime was found";
        return SteamVrState::kUnavailable;
    }
    vr::EVRInitError error = vr::VRInitError_None;
    vr::VR_Init(&error, vr::VRApplication_Background);
    if (error == vr::VRInitError_None) {
        vr::VR_Shutdown();
        return SteamVrState::kRunning;
    }
    if (error == vr::VRInitError_Init_NoServerForBackgroundApp) return SteamVrState::kNotRunning;
    detail = std::string(vr::VR_GetVRInitErrorAsSymbol(error)) + " (" +
             vr::VR_GetVRInitErrorAsEnglishDescription(error) + ")";
    return SteamVrState::kUnavailable;
}

bool Runtime::initialize() {
    if (initialized_) {
        return true;
    }

    const bool runtime_installed = vr::VR_IsRuntimeInstalled();
    std::cout << "[OpenVR] Runtime installed: " << (runtime_installed ? "yes" : "no") << '\n';
    if (!runtime_installed) {
        std::cerr << "[OpenVR] No registered OpenVR runtime was found\n";
        return false;
    }

    std::array<char, 4096> runtime_path{};
    std::uint32_t required_size = 0;
    if (vr::VR_GetRuntimePath(runtime_path.data(), static_cast<std::uint32_t>(runtime_path.size()),
                              &required_size) &&
        required_size > 0 && required_size <= runtime_path.size()) {
        std::cout << "[OpenVR] Runtime path: " << runtime_path.data() << '\n';
    } else {
        std::cout << "[OpenVR] Runtime path: unavailable (required bytes: " << required_size << ")\n";
    }

    vr::EVRInitError init_error = vr::VRInitError_None;
    system_ = vr::VR_Init(&init_error, vr::VRApplication_Overlay);
    if (init_error != vr::VRInitError_None || system_ == nullptr) {
        std::cerr << "[OpenVR] Initialization failed: "
                  << vr::VR_GetVRInitErrorAsSymbol(init_error) << " ("
                  << vr::VR_GetVRInitErrorAsEnglishDescription(init_error) << ")\n";
        system_ = nullptr;
        return false;
    }
    initialized_ = true;

    vr::EVRInitError interface_error = vr::VRInitError_None;
    overlay_ = static_cast<vr::IVROverlay*>(
        vr::VR_GetGenericInterface(vr::IVROverlay_Version, &interface_error));
    if (interface_error != vr::VRInitError_None || overlay_ == nullptr) {
        std::cerr << "[OpenVR] IVROverlay unavailable: "
                  << vr::VR_GetVRInitErrorAsSymbol(interface_error) << " ("
                  << vr::VR_GetVRInitErrorAsEnglishDescription(interface_error) << ")\n";
        return false;
    }

    interface_error = vr::VRInitError_None;
    notifications_ = static_cast<vr::IVRNotifications*>(
        vr::VR_GetGenericInterface(vr::IVRNotifications_Version, &interface_error));
    if (interface_error != vr::VRInitError_None || notifications_ == nullptr) {
        notifications_ = nullptr;
        std::cout << "[OpenVR] Notifications interface unavailable: "
                  << vr::VR_GetVRInitErrorAsSymbol(interface_error) << " ("
                  << vr::VR_GetVRInitErrorAsEnglishDescription(interface_error) << ")\n";
    }

    const char* runtime_version = system_->GetRuntimeVersion();
    std::cout << "[OpenVR] Runtime initialized\n"
              << "[OpenVR] Runtime version: "
              << (runtime_version != nullptr ? runtime_version : "unknown") << '\n'
              << "[OpenVR] SDK version: " << vr::k_nSteamVRVersionMajor << '.'
              << vr::k_nSteamVRVersionMinor << '.' << vr::k_nSteamVRVersionBuild << '\n'
              << "[OpenVR] HMD connected: "
              << (system_->IsTrackedDeviceConnected(vr::k_unTrackedDeviceIndex_Hmd) ? "yes" : "no")
              << '\n'
              << "[OpenVR] Application type: Overlay\n"
              << "[OpenVR] Overlay interface: " << vr::IVROverlay_Version << '\n'
              << "[OpenVR] Notifications interface: "
              << (notifications_ != nullptr ? vr::IVRNotifications_Version : "unavailable")
              << '\n';

    return true;
}

}  // namespace frame_notify::openvr
