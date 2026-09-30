#include "system/single_instance.h"

#include <cerrno>
#include <cstdlib>
#include <cstring>

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

namespace frame_notify::system {

SingleInstance::~SingleInstance() {
    if (descriptor_ >= 0) ::close(descriptor_);   // closing also drops the lock
}

std::string SingleInstance::default_directory() {
    const char* runtime_directory = std::getenv("XDG_RUNTIME_DIR");
    return runtime_directory != nullptr ? std::string(runtime_directory) : std::string();
}

bool SingleInstance::acquire(const std::string& directory, const std::string& name) {
    error_.clear();
    holder_ = 0;
    if (descriptor_ >= 0) return true;
    if (directory.empty()) {
        error_ = "XDG_RUNTIME_DIR is not set";
        return false;
    }
    const std::string path = directory + "/" + name;
    // O_CLOEXEC: the Bluetooth helper and every other child must not inherit, and so keep, the lock.
    const int descriptor = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, S_IRUSR | S_IWUSR);
    if (descriptor < 0) {
        error_ = "cannot open " + path + ": " + std::strerror(errno);
        return false;
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const int reason = errno;
        char buffer[32] = {};
        const ssize_t count = ::pread(descriptor, buffer, sizeof(buffer) - 1U, 0);
        if (count > 0) holder_ = std::strtol(buffer, nullptr, 10);
        ::close(descriptor);
        error_ = reason == EWOULDBLOCK ? "another Frame Notify is already running"
                                       : "cannot lock " + path + ": " + std::strerror(reason);
        return false;
    }
    // Leave the process id behind, only so a refused second start can say who has the lock.
    const std::string pid = std::to_string(::getpid()) + "\n";
    if (::ftruncate(descriptor, 0) == 0) {
        [[maybe_unused]] const ssize_t written = ::pwrite(descriptor, pid.data(), pid.size(), 0);
    }
    descriptor_ = descriptor;
    return true;
}

}  // namespace frame_notify::system
