#pragma once

#include "openvr/runtime.h"

#include <chrono>
#include <functional>
#include <string>

namespace frame_notify::openvr {

// Decides when Frame Notify should connect to SteamVR while it runs in the background: it asks
// now and then whether SteamVR is up, says what it is waiting for (once, not every time it asks),
// and will not connect to a SteamVR that is still shutting down from the session before.
class AttachPlan {
public:
    using Probe = std::function<SteamVrState(std::string& detail)>;
    using Log = std::function<void(const std::string& line)>;

    struct Options {
        std::chrono::milliseconds interval{5000};        // between questions
        std::chrono::milliseconds initial_delay{0};       // before the first question
        bool wait_for_shutdown_first = false;             // SteamVR has just quit: it must be seen gone first
    };

    AttachPlan(Probe probe, Options options, Log log, std::chrono::steady_clock::time_point now);

    // Call regularly while not connected. True on the call when it is time to connect (SteamVR is up).
    [[nodiscard]] bool poll(std::chrono::steady_clock::time_point now);

private:
    Probe probe_;
    Options options_;
    Log log_;
    std::chrono::steady_clock::time_point next_question_;
    bool seen_down_;
    std::string announced_;   // the last thing said about waiting, so it is not said again
};

}  // namespace frame_notify::openvr
