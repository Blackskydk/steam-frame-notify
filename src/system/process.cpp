#include "system/process.h"

#include <algorithm>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <thread>

#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

extern char** environ;

namespace frame_notify::system {
namespace {

bool has_prefix(const std::string& text, const std::string& prefix) {
    return text.compare(0, prefix.size(), prefix) == 0;
}

std::string trimmed(std::string text) {
    while (!text.empty() && (text.back() == '\n' || text.back() == '\r' || text.back() == ' ')) text.pop_back();
    return text;
}

}  // namespace

AsyncProcess::~AsyncProcess() {
    stop_program();
}

bool AsyncProcess::start(const std::vector<std::string>& command,
                         const std::vector<std::pair<std::string, std::string>>& default_environment,
                         std::chrono::milliseconds limit, std::size_t maximum_output) {
    stop_program();
    result_ = ProcessResult{};
    maximum_output_ = maximum_output;
    if (command.empty()) return false;

    std::vector<std::string> environment;
    for (char** entry = environ; entry != nullptr && *entry != nullptr; ++entry) environment.emplace_back(*entry);
    for (const auto& [name, value] : default_environment) {
        const std::string prefix = name + "=";
        const bool present = std::any_of(environment.begin(), environment.end(), [&prefix](const std::string& item) {
            return has_prefix(item, prefix) && item.size() > prefix.size();
        });
        if (!present) environment.push_back(prefix + value);
    }
    std::vector<char*> envp;
    for (auto& item : environment) envp.push_back(item.data());
    envp.push_back(nullptr);

    std::vector<std::string> arguments = command;
    std::vector<char*> argv;
    for (auto& item : arguments) argv.push_back(item.data());
    argv.push_back(nullptr);

    int output[2] = {-1, -1};
    if (::pipe2(output, O_CLOEXEC) != 0) {
        result_.output = std::string("cannot run ") + command[0] + ": " + std::strerror(errno);
        return false;
    }
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, output[1], STDERR_FILENO);
    pid_t child = 0;
    const int spawn_error = ::posix_spawnp(&child, argv[0], &actions, nullptr, argv.data(), envp.data());
    posix_spawn_file_actions_destroy(&actions);
    ::close(output[1]);
    if (spawn_error != 0) {
        ::close(output[0]);
        result_.output = "cannot run " + command[0] + ": " + std::strerror(spawn_error);
        return false;
    }
    result_.started = true;
    pid_ = child;
    output_ = output[0];
    ::fcntl(output_, F_SETFL, ::fcntl(output_, F_GETFL) | O_NONBLOCK);
    deadline_ = std::chrono::steady_clock::now() + limit;
    return true;
}

void AsyncProcess::drain() {
    char chunk[512];
    while (output_ >= 0) {
        const ssize_t count = ::read(output_, chunk, sizeof(chunk));
        if (count > 0) {
            if (result_.output.size() < maximum_output_) result_.output.append(chunk, static_cast<std::size_t>(count));
            continue;
        }
        if (count < 0 && errno == EINTR) continue;
        break;
    }
}

void AsyncProcess::stop_program() {
    if (pid_ > 0) {
        ::kill(pid_, SIGKILL);
        int status = 0;
        ::waitpid(pid_, &status, 0);
        pid_ = 0;
    }
    if (output_ >= 0) {
        ::close(output_);
        output_ = -1;
    }
}

bool AsyncProcess::poll() {
    if (pid_ <= 0) return true;
    drain();
    int wait_status = 0;
    const pid_t done = ::waitpid(pid_, &wait_status, WNOHANG);
    bool finished = false;
    if (done == pid_ || (done < 0 && errno != EINTR)) {
        finished = true;
        drain();
        if (done == pid_ && WIFEXITED(wait_status)) result_.exit_code = WEXITSTATUS(wait_status);
        pid_ = 0;
    } else if (std::chrono::steady_clock::now() >= deadline_) {
        ::kill(pid_, SIGKILL);
        ::waitpid(pid_, &wait_status, 0);
        pid_ = 0;
        result_.timed_out = true;
        finished = true;
    }
    if (!finished) return false;

    if (output_ >= 0) {
        ::close(output_);
        output_ = -1;
    }
    result_.output = trimmed(std::move(result_.output));
    if (result_.output.size() > maximum_output_) result_.output.resize(maximum_output_);
    if (result_.timed_out) result_.output += result_.output.empty() ? "(timed out)" : " (timed out)";
    return true;
}

ProcessResult run_process(const std::vector<std::string>& command,
                          const std::vector<std::pair<std::string, std::string>>& default_environment,
                          std::chrono::milliseconds limit, std::size_t maximum_output) {
    AsyncProcess process;
    if (!process.start(command, default_environment, limit, maximum_output)) return process.result();
    while (!process.poll()) std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return process.result();
}

}  // namespace frame_notify::system
