#pragma once

#include "system/process.h"

#include <chrono>
#include <string>

namespace frame_notify::system {

enum class InstallState {
    kIdle,         // nothing started
    kInstalling,   // the installer is running
    kDone,         // the new version is in place; the program should start it
    kFailed,       // see `message`
};

struct InstallStatus {
    InstallState state = InstallState::kIdle;
    std::string message;   // why it failed
};

// Installs a newer release in place, without blocking, by running the installer that was put next to
// the program (install.sh) the way a person would: it downloads the release from GitHub, checks its
// checksum, and replaces the program's files. The program keeps running meanwhile (--keep-running:
// the installer must not stop the service, which would end it too) and restarts itself once start()
// has led to kDone. It only runs when told to, never on its own.
class UpdateInstaller {
public:
    struct Options {
        std::string script;   // install.sh of this copy; empty when this copy cannot update itself
        std::string bash = "bash";
        std::chrono::milliseconds limit = std::chrono::minutes(10);
    };

    explicit UpdateInstaller(Options options);

    // Whether this copy can update itself at all.
    [[nodiscard]] bool available() const noexcept { return !options_.script.empty(); }
    // Starts installing `version` ("0.1.4"). `autostart` says whether Frame Notify starts with the
    // Frame now, so an update does not turn back on what was turned off. False, and nothing changes,
    // while an install is running; a failure to even begin is reported through status().
    bool start(const std::string& version, bool autostart);
    // Call regularly. True on the call that finds the install over (then status() is final).
    [[nodiscard]] bool poll();
    [[nodiscard]] const InstallStatus& status() const noexcept { return status_; }

    // The installer of the copy of the program at `executable`: install.sh beside it, but only when
    // that folder is `app_dir`, where the installer puts the program. A copy run from anywhere else
    // (a build, an unpacked download) would not be the one that gets replaced. Empty when not.
    [[nodiscard]] static std::string find_script(const std::string& executable, const std::string& app_dir);
    // Where the installer puts the program: $FRAME_NOTIFY_APP_DIR, or ~/.local/share/frame-notify.
    [[nodiscard]] static std::string default_app_dir();
    // A version number safe to hand to the installer: digits, letters, dots, dashes and underscores.
    [[nodiscard]] static bool valid_version(const std::string& version);

private:
    void fail(std::string message);

    Options options_;
    AsyncProcess process_;
    InstallStatus status_;
};

}  // namespace frame_notify::system
