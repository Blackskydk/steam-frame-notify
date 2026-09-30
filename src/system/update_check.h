#pragma once

#include "system/process.h"

#include <chrono>
#include <optional>
#include <string>

namespace frame_notify::system {

enum class UpdateState {
    kIdle,        // never asked
    kChecking,    // waiting for the answer
    kCurrent,     // this is the newest release (or newer)
    kAvailable,   // a newer release exists
    kUnknown,     // the newest release is known, but this build has no version to compare with
    kFailed,      // could not find out; see `message`
};

struct UpdateStatus {
    UpdateState state = UpdateState::kIdle;
    std::string latest;    // the newest release's version without a leading "v", when known
    std::string message;   // why it failed
};

// Finds out whether a newer release of Frame Notify exists, without blocking: it asks GitHub which
// release is the newest by following the repository's "releases/latest" address with `curl`, which
// redirects to the page of that release, and reads the version off the address it ends up at. It
// only asks when told to (start()), never on its own.
class UpdateChecker {
public:
    struct Options {
        std::string current_version;                  // what is running, for example "0.1.1"
        std::string releases_url;                     // https://github.com/<owner>/<repo>/releases/latest
        std::string curl = "curl";
        std::chrono::milliseconds limit = std::chrono::seconds(20);
    };

    explicit UpdateChecker(Options options);

    // Starts asking. False, and nothing changes, while an answer is still awaited.
    bool start();
    // Call regularly. True on the call that finds the answer has arrived (then status() is final).
    [[nodiscard]] bool poll();
    [[nodiscard]] const UpdateStatus& status() const noexcept { return status_; }

    // "https://github.com/o/r/releases/tag/v1.2.3" -> "v1.2.3"; empty when it is not a release page.
    [[nodiscard]] static std::string tag_from_url(const std::string& url);
    // -1, 0 or 1 for a below, equal to or above b. Versions are dotted numbers with an optional
    // leading "v"; a "-suffix" (such as -rc1 or a commit id) ranks below the same number without one.
    // Empty when either is not a version.
    [[nodiscard]] static std::optional<int> compare_versions(const std::string& a, const std::string& b);
    // "v1.2.3" -> "1.2.3"
    [[nodiscard]] static std::string version_from_tag(const std::string& tag);

private:
    Options options_;
    AsyncProcess process_;
    UpdateStatus status_;
};

}  // namespace frame_notify::system
