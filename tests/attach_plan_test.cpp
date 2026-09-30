#include "openvr/attach_plan.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <string>
#include <vector>

// When Frame Notify asks whether SteamVR is up, what it says about waiting, and the rule that it
// does not reconnect to a SteamVR that is still shutting down.

namespace {

using namespace frame_notify::openvr;
using namespace std::chrono_literals;
using Clock = std::chrono::steady_clock;

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "attach_plan_test.cpp:" << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(condition) expect((condition), #condition, __LINE__)

// A scripted SteamVR: what each question is answered with.
struct Script {
    std::vector<SteamVrState> answers;
    std::string detail = "reason";
    int asked = 0;

    AttachPlan::Probe probe() {
        return [this](std::string& out) {
            out = detail;
            const SteamVrState answer = answers.empty() ? SteamVrState::kNotRunning
                                                        : answers[std::min<std::size_t>(asked, answers.size() - 1U)];
            ++asked;
            return answer;
        };
    }
};

}  // namespace

int main() {
    const Clock::time_point start = Clock::now();

    // Waiting for SteamVR: asks every interval, says so once, never connects.
    {
        Script script{{SteamVrState::kNotRunning}};
        std::vector<std::string> said;
        AttachPlan plan(script.probe(), {}, [&](const std::string& line) { said.push_back(line); }, start);
        EXPECT(!plan.poll(start));
        EXPECT(script.asked == 1 && said.size() == 1U && said[0] == "Waiting for SteamVR to start");
        EXPECT(!plan.poll(start + 1s) && !plan.poll(start + 4999ms));   // too soon to ask again
        EXPECT(script.asked == 1);
        EXPECT(!plan.poll(start + 5s));
        EXPECT(script.asked == 2);
        for (int second = 10; second <= 600; second += 5) EXPECT(!plan.poll(start + std::chrono::seconds(second)));
        EXPECT(script.asked == 2 + 119);                                 // one question per interval, no more
        EXPECT(said.size() == 1U);                                       // and the news is not repeated
    }

    // SteamVR comes up: connect on that very question; it is not announced as waiting again.
    {
        Script script{{SteamVrState::kNotRunning, SteamVrState::kNotRunning, SteamVrState::kRunning}};
        std::vector<std::string> said;
        AttachPlan plan(script.probe(), {}, [&](const std::string& line) { said.push_back(line); }, start);
        EXPECT(!plan.poll(start) && !plan.poll(start + 5s));
        EXPECT(plan.poll(start + 10s));
        EXPECT(said.size() == 1U);
    }

    // SteamVR is already running when Frame Notify starts: connect at once.
    {
        Script script{{SteamVrState::kRunning}};
        std::vector<std::string> said;
        AttachPlan plan(script.probe(), {}, [&](const std::string& line) { said.push_back(line); }, start);
        EXPECT(plan.poll(start) && said.empty());
    }

    // A delay before the first question, and a custom interval.
    {
        Script script{{SteamVrState::kNotRunning}};
        AttachPlan::Options options;
        options.initial_delay = 30s;
        options.interval = 2s;
        AttachPlan plan(script.probe(), options, nullptr, start);
        EXPECT(!plan.poll(start) && !plan.poll(start + 29s) && script.asked == 0);
        EXPECT(!plan.poll(start + 30s) && script.asked == 1);
        EXPECT(!plan.poll(start + 31s) && script.asked == 1);
        EXPECT(!plan.poll(start + 32s) && script.asked == 2);
    }

    // Right after a session SteamVR may still answer "running" while it shuts down. It must be seen
    // gone before a new connection is made, or Frame Notify would reconnect to a dying SteamVR.
    {
        Script script{{SteamVrState::kRunning, SteamVrState::kRunning, SteamVrState::kNotRunning,
                       SteamVrState::kRunning}};
        AttachPlan::Options options;
        options.wait_for_shutdown_first = true;
        std::vector<std::string> said;
        AttachPlan plan(script.probe(), options, [&](const std::string& line) { said.push_back(line); }, start);
        EXPECT(!plan.poll(start));
        EXPECT(said.size() == 1U && said[0].find("still shutting down") != std::string::npos);
        EXPECT(!plan.poll(start + 5s) && said.size() == 1U);             // same news, said once
        EXPECT(!plan.poll(start + 10s));                                 // gone: now waiting for the next start
        EXPECT(said.size() == 2U && said[1] == "Waiting for SteamVR to start");
        EXPECT(plan.poll(start + 15s));                                  // up again: connect
    }

    // OpenVR not usable: the reason is told once per change, and there is no connecting.
    {
        Script script{{SteamVrState::kUnavailable, SteamVrState::kUnavailable, SteamVrState::kUnavailable,
                       SteamVrState::kNotRunning, SteamVrState::kUnavailable}};
        std::vector<std::string> said;
        AttachPlan plan(script.probe(), {}, [&](const std::string& line) { said.push_back(line); }, start);
        EXPECT(!plan.poll(start) && !plan.poll(start + 5s) && !plan.poll(start + 10s));
        EXPECT(said.size() == 1U && said[0] == "OpenVR is not usable yet: reason");
        script.detail = "something else";
        EXPECT(!plan.poll(start + 15s));                                 // not running
        EXPECT(!plan.poll(start + 20s));                                 // unavailable again, new reason
        EXPECT(said.size() == 3U && said[2] == "OpenVR is not usable yet: something else");
        Script silent{{SteamVrState::kUnavailable}, ""};
        std::vector<std::string> blank;
        AttachPlan no_reason(silent.probe(), {}, [&](const std::string& line) { blank.push_back(line); }, start);
        EXPECT(!no_reason.poll(start) && blank.size() == 1U && blank[0].find("no reason given") != std::string::npos);
    }

    if (failures != 0) {
        std::cerr << failures << " expectation(s) failed\n";
        return 1;
    }
    return 0;
}
