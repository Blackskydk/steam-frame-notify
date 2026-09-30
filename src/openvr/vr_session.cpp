#include "openvr/vr_session.h"

#include "system/process.h"

#include <iostream>
#include <utility>

namespace frame_notify::openvr {
namespace {

// A probe that takes longer than this is treated as "cannot tell": the program's other work waits for it.
constexpr auto kProbeLimit = std::chrono::seconds(4);

}  // namespace

VrSession::VrSession(Options options)
    : options_(std::move(options)),
      plan_([this](std::string& detail) { return probe(detail); }, options_.plan,
            [](const std::string& line) { std::cout << "[VR] " << line << '\n'; },
            std::chrono::steady_clock::now()) {}

VrSession::~VrSession() {
    detach();
}

SteamVrState VrSession::probe(std::string& detail) const {
    if (options_.executable.empty()) return Runtime::probe(detail);   // cannot start a probe process
    const system::ProcessResult result =
        system::run_process({options_.executable, "--probe-steamvr"}, {}, kProbeLimit);
    if (result.started && result.exit_code == 0) return SteamVrState::kRunning;
    if (result.started && result.exit_code == 1) return SteamVrState::kNotRunning;
    detail = result.output.empty() ? std::string("the SteamVR probe did not finish") : result.output;
    return SteamVrState::kUnavailable;
}

bool VrSession::poll(std::chrono::steady_clock::time_point now) {
    if (attached() || failed_) return false;
    if (!plan_.poll(now)) return false;
    std::cout << "[VR] SteamVR is running; connecting\n";
    return attach();
}

bool VrSession::attach() {
    runtime_ = std::make_unique<Runtime>();
    if (!runtime_->initialize()) {
        failed_ = true;
        return false;
    }
    dashboard_ = std::make_unique<Dashboard>();
    if (!dashboard_->create(runtime_->overlay())) {
        failed_ = true;
        return false;
    }
    notification_ = std::make_unique<NativeNotification>();
    if (!notification_->bind(runtime_->notifications(), dashboard_->main_handle())) {
        failed_ = true;
        return false;
    }
    std::cout << "[VR] Connected to SteamVR\n";
    return true;
}

void VrSession::detach() {
    // The overlays must go while OpenVR is still connected.
    notification_.reset();
    dashboard_.reset();
    runtime_.reset();
}

}  // namespace frame_notify::openvr
