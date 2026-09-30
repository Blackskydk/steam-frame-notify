#include "openvr/attach_plan.h"

#include <utility>

namespace frame_notify::openvr {

AttachPlan::AttachPlan(Probe probe, Options options, Log log, std::chrono::steady_clock::time_point now)
    : probe_(std::move(probe)),
      options_(options),
      log_(std::move(log)),
      next_question_(now + options.initial_delay),
      seen_down_(!options.wait_for_shutdown_first) {}

bool AttachPlan::poll(std::chrono::steady_clock::time_point now) {
    if (now < next_question_) return false;
    next_question_ = now + options_.interval;

    std::string detail;
    const SteamVrState state = probe_(detail);
    std::string saying;
    bool connect = false;
    switch (state) {
    case SteamVrState::kNotRunning:
        seen_down_ = true;
        saying = "Waiting for SteamVR to start";
        break;
    case SteamVrState::kUnavailable:
        saying = "OpenVR is not usable yet: " + (detail.empty() ? std::string("no reason given") : detail);
        break;
    case SteamVrState::kRunning:
        if (seen_down_) {
            connect = true;
        } else {
            saying = "SteamVR is still shutting down; waiting for it to be gone before connecting again";
        }
        break;
    }
    if (!saying.empty() && saying != announced_) {
        announced_ = saying;
        if (log_) log_(saying);
    }
    if (connect) announced_.clear();   // whatever comes next is news again
    return connect;
}

}  // namespace frame_notify::openvr
