#include "bluetooth/phone_link.h"

#include "ipc/json_line.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <thread>

#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace frame_notify::bluetooth {
namespace {

constexpr std::size_t kMaximumBufferedBytes = 1U << 20;
constexpr int kMaximumQuickFailures = 5;
constexpr auto kHealthyRunTime = std::chrono::seconds(60);

std::string describe_exit(int wait_status) {
    if (WIFEXITED(wait_status)) return "exit code " + std::to_string(WEXITSTATUS(wait_status));
    if (WIFSIGNALED(wait_status)) return "signal " + std::to_string(WTERMSIG(wait_status));
    return "unknown reason";
}

void close_descriptor(int& descriptor) noexcept {
    if (descriptor >= 0) {
        ::close(descriptor);
        descriptor = -1;
    }
}

}  // namespace

std::string PhoneStatus::field(const std::string& name, const std::string& fallback) const {
    const auto found = fields.find(name);
    return found == fields.end() ? fallback : found->second;
}

PhoneLink::~PhoneLink() {
    stop();
}

void PhoneLink::set_state(std::string state, std::map<std::string, std::string> fields) {
    if (status_.state == state && status_.fields == fields) return;
    status_.state = std::move(state);
    status_.fields = std::move(fields);
    status_.since = std::chrono::steady_clock::now();
    status_changed_ = true;
}

void PhoneLink::give_up(const std::string& message) {
    restart_due_ = false;
    set_state("helper_unavailable", {{"message", message}});
}

bool PhoneLink::start(std::vector<std::string> command) {
    stop();   // a helper that is already running is replaced, not leaked
    command_ = std::move(command);
    quick_failures_ = 0;
    stopped_ = false;
    if (command_.empty()) {
        give_up("The Bluetooth helper script was not found next to Frame Notify.");
        return false;
    }
    return spawn();
}

bool PhoneLink::spawn() {
    // Writing to a helper that has just exited must fail with an error, not kill Frame Notify.
    std::signal(SIGPIPE, SIG_IGN);

    int input[2] = {-1, -1};    // parent writes input[1], child reads input[0]
    int output[2] = {-1, -1};   // child writes output[1], parent reads output[0]
    if (::pipe2(input, O_CLOEXEC) != 0 || ::pipe2(output, O_CLOEXEC) != 0) {
        const std::string reason = std::strerror(errno);
        for (const int descriptor : {input[0], input[1], output[0], output[1]}) {
            if (descriptor >= 0) ::close(descriptor);
        }
        give_up("Could not create pipes for the Bluetooth helper: " + reason);
        return false;
    }

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, input[0], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);

    std::vector<char*> argv;
    argv.reserve(command_.size() + 1U);
    for (auto& argument : command_) argv.push_back(argument.data());
    argv.push_back(nullptr);

    pid_t child = 0;
    const int error = ::posix_spawnp(&child, command_[0].c_str(), &actions, nullptr, argv.data(),
                                     environ);
    posix_spawn_file_actions_destroy(&actions);
    ::close(input[0]);
    ::close(output[1]);
    if (error != 0) {
        ::close(input[1]);
        ::close(output[0]);
        give_up("Could not start " + command_[0] + ": " + std::strerror(error));
        return false;
    }

    pid_ = child;
    to_child_ = input[1];
    from_child_ = output[0];
    ::fcntl(to_child_, F_SETFL, ::fcntl(to_child_, F_GETFL) | O_NONBLOCK);
    ::fcntl(from_child_, F_SETFL, ::fcntl(from_child_, F_GETFL) | O_NONBLOCK);
    buffer_.clear();
    fatal_message_.clear();
    started_at_ = std::chrono::steady_clock::now();
    restart_due_ = false;
    set_state("starting", {});
    return true;
}

void PhoneLink::read_output() {
    if (from_child_ < 0) return;
    char chunk[4096];
    while (true) {
        const ssize_t count = ::read(from_child_, chunk, sizeof(chunk));
        if (count > 0) {
            buffer_.append(chunk, static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        if (count == 0 || (errno != EAGAIN && errno != EWOULDBLOCK)) close_descriptor(from_child_);
        break;
    }
    process_lines();
}

void PhoneLink::process_lines() {
    std::size_t start = 0;
    for (std::size_t newline = buffer_.find('\n', start); newline != std::string::npos;
         newline = buffer_.find('\n', start)) {
        std::string line = buffer_.substr(start, newline - start);
        start = newline + 1U;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::string error;
        auto fields = ipc::parse_flat_json(line, error);
        if (!fields) {
            if (!line.empty()) std::cerr << "[Bluetooth] Ignoring unreadable helper output\n";
            continue;
        }
        const std::string event = fields->count("event") != 0U ? (*fields)["event"] : std::string();
        if (event == "state" && fields->count("state") != 0U) {
            std::string state = (*fields)["state"];
            fields->erase("event");
            fields->erase("state");
            set_state(std::move(state), std::move(*fields));
        } else if (event == "fatal") {
            fatal_message_ = fields->count("message") != 0U ? (*fields)["message"]
                                                            : std::string("The Bluetooth helper stopped.");
        }
    }
    buffer_.erase(0, start);
    if (buffer_.size() > kMaximumBufferedBytes) buffer_.clear();  // a line that never ends
}

bool PhoneLink::reap() {
    if (pid_ <= 0) return false;
    int wait_status = 0;
    const pid_t result = ::waitpid(pid_, &wait_status, WNOHANG);
    if (result == 0) return false;
    if (result < 0 && errno == EINTR) return false;

    read_output();           // whatever it said just before exiting, such as a fatal message
    close_pipes();
    pid_ = 0;
    if (!fatal_message_.empty()) {
        give_up(fatal_message_);
        return true;
    }

    const std::string reason = result < 0 ? "unknown reason" : describe_exit(wait_status);
    if (std::chrono::steady_clock::now() - started_at_ >= kHealthyRunTime) quick_failures_ = 0;
    ++quick_failures_;
    if (quick_failures_ > kMaximumQuickFailures) {
        give_up("The Bluetooth helper keeps stopping (" + reason +
                "). The terminal Frame Notify runs in may show why.");
        return true;
    }
    auto delay = backoff_first_;
    for (int step = 1; step < quick_failures_ && delay < backoff_maximum_; ++step) delay *= 2;
    restart_at_ = std::chrono::steady_clock::now() + std::min(delay, backoff_maximum_);
    restart_due_ = true;
    set_state("helper_restarting", {{"reason", reason}});
    return true;
}

bool PhoneLink::poll() {
    if (pid_ > 0) {
        read_output();
        reap();
    } else if (restart_due_ && !stopped_ && std::chrono::steady_clock::now() >= restart_at_) {
        spawn();
    }
    return std::exchange(status_changed_, false);
}

bool PhoneLink::send(std::string_view command,
                     const std::vector<std::pair<std::string, std::string>>& fields) {
    if (pid_ <= 0 || to_child_ < 0) return false;
    std::vector<std::pair<std::string, std::string>> all;
    all.emplace_back("command", std::string(command));
    all.insert(all.end(), fields.begin(), fields.end());
    const std::string line = ipc::serialize_flat_json(all) + "\n";

    std::size_t written = 0;
    while (written < line.size()) {
        const ssize_t count = ::write(to_child_, line.data() + written, line.size() - written);
        if (count > 0) {
            written += static_cast<std::size_t>(count);
        } else if (count < 0 && errno == EINTR) {
            continue;
        } else {
            return false;   // the pipe is full or closed: the command is dropped, not half-sent later
        }
    }
    return true;
}

void PhoneLink::retry() {
    if (pid_ > 0) {
        send("retry");
        return;
    }
    if (command_.empty()) return;
    quick_failures_ = 0;
    stopped_ = false;
    spawn();
}

void PhoneLink::close_pipes() noexcept {
    close_descriptor(to_child_);
    close_descriptor(from_child_);
}

void PhoneLink::stop() {
    stopped_ = true;
    restart_due_ = false;
    if (pid_ <= 0) {
        close_pipes();
        return;
    }
    send("quit");
    close_descriptor(to_child_);   // end of input also tells the helper to leave

    const auto wait_for_exit = [this](std::chrono::milliseconds limit) {
        const auto deadline = std::chrono::steady_clock::now() + limit;
        while (true) {
            int wait_status = 0;
            const pid_t result = ::waitpid(pid_, &wait_status, WNOHANG);
            if (result == pid_ || (result < 0 && errno != EINTR)) return true;
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    };
    bool exited = wait_for_exit(quit_timeout_);
    if (!exited) {
        ::kill(pid_, SIGTERM);
        exited = wait_for_exit(terminate_timeout_);
    }
    if (!exited) {
        ::kill(pid_, SIGKILL);
        int wait_status = 0;
        while (::waitpid(pid_, &wait_status, 0) < 0 && errno == EINTR) {
        }
    }
    close_pipes();
    pid_ = 0;
}

std::string find_helper_script() {
    namespace fs = std::filesystem;
    std::error_code error;
    if (const char* configured = std::getenv("FRAME_NOTIFY_BRIDGE");
        configured != nullptr && *configured != '\0') {
        return fs::exists(configured, error) ? std::string(configured) : std::string();
    }

    std::vector<fs::path> candidates;
    const fs::path executable = fs::read_symlink("/proc/self/exe", error);
    if (!error && executable.has_parent_path()) {
        candidates.push_back(executable.parent_path() / ".." / "scripts" / "ancs_bridge.py");
        candidates.push_back(executable.parent_path() / "scripts" / "ancs_bridge.py");
    }
#ifdef FRAME_NOTIFY_SCRIPTS_DIR
    candidates.push_back(fs::path(FRAME_NOTIFY_SCRIPTS_DIR) / "ancs_bridge.py");
#endif
    candidates.emplace_back("scripts/ancs_bridge.py");
    candidates.emplace_back("../scripts/ancs_bridge.py");
    for (const auto& candidate : candidates) {
        error.clear();
        if (fs::exists(candidate, error) && !error) {
            const fs::path canonical = fs::weakly_canonical(candidate, error);
            return (error ? candidate : canonical).string();
        }
    }
    return {};
}

std::vector<std::string> default_helper_command() {
    const std::string script = find_helper_script();
    if (script.empty()) return {};
    const char* python = std::getenv("FRAME_NOTIFY_PYTHON");
    return {python != nullptr && *python != '\0' ? python : "python3", script, "--service"};
}

}  // namespace frame_notify::bluetooth
