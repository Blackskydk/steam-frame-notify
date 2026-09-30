#pragma once

#include "openvr/attach_plan.h"
#include "openvr/dashboard.h"
#include "openvr/notifications.h"
#include "openvr/runtime.h"

#include <chrono>
#include <memory>
#include <string>

namespace frame_notify::openvr {

// Frame Notify's connection to SteamVR, for a program that runs in the background whether SteamVR
// does or not. While SteamVR is not running there is nothing here; once it is, the dashboard entry
// and the toasts come to life. It connects at most once per process: when that session ends the
// program starts over (see main.cpp), because connecting to SteamVR again after having
// disconnected is not reliable.
class VrSession {
public:
    struct Options {
        std::string executable;   // this program; run with --probe-steamvr to ask whether SteamVR is up
        AttachPlan::Options plan;
    };

    explicit VrSession(Options options);
    ~VrSession();

    VrSession(const VrSession&) = delete;
    VrSession& operator=(const VrSession&) = delete;

    // While waiting: connects when SteamVR is running. True on the call that connected.
    bool poll(std::chrono::steady_clock::time_point now);
    // Connected and usable.
    [[nodiscard]] bool attached() const noexcept { return notification_ != nullptr && !failed_; }
    // SteamVR was running, but connecting to it did not work.
    [[nodiscard]] bool failed() const noexcept { return failed_; }
    // Takes the overlays down and disconnects. Safe to call at any time, also twice.
    void detach();

    [[nodiscard]] Runtime& runtime() { return *runtime_; }
    [[nodiscard]] Dashboard& dashboard() { return *dashboard_; }
    [[nodiscard]] NativeNotification& notification() { return *notification_; }

private:
    [[nodiscard]] SteamVrState probe(std::string& detail) const;
    bool attach();

    Options options_;
    AttachPlan plan_;
    std::unique_ptr<Runtime> runtime_;
    std::unique_ptr<Dashboard> dashboard_;
    std::unique_ptr<NativeNotification> notification_;
    bool failed_ = false;
};

}  // namespace frame_notify::openvr
