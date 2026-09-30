#pragma once

#include <string>

namespace frame_notify::system {

// Makes sure only one Frame Notify runs per user: two of them would fight over the notification
// socket, the Bluetooth connection and the history file. Holds a lock on a file in the user's
// runtime directory for as long as the object lives; the system releases it when the process ends,
// however it ends.
class SingleInstance {
public:
    SingleInstance() = default;
    ~SingleInstance();

    SingleInstance(const SingleInstance&) = delete;
    SingleInstance& operator=(const SingleInstance&) = delete;

    // Takes the lock. Returns false when another process holds it (see holder()) or the lock file
    // cannot be used (see error()).
    bool acquire(const std::string& directory, const std::string& name = "frame-notify.lock");
    // $XDG_RUNTIME_DIR, or an empty string when it is not set.
    [[nodiscard]] static std::string default_directory();

    [[nodiscard]] bool held() const noexcept { return descriptor_ >= 0; }
    // The process id the current holder wrote into the lock file, or 0 when unknown.
    [[nodiscard]] long holder() const noexcept { return holder_; }
    [[nodiscard]] const std::string& error() const noexcept { return error_; }

private:
    int descriptor_ = -1;
    long holder_ = 0;
    std::string error_;
};

}  // namespace frame_notify::system
