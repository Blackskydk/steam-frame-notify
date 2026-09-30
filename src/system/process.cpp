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

ProcessResult run_process(const std::vector<std::string>& command,
                          const std::vector<std::pair<std::string, std::string>>& default_environment,
                          std::chrono::milliseconds limit, std::size_t maximum_output) {
    ProcessResult result;
    if (command.empty()) return result;

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
    if (::pipe2(output, O_CLOEXEC) != 0) return result;
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
        result.output = "cannot run " + command[0] + ": " + std::strerror(spawn_error);
        return result;
    }
    result.started = true;
    ::fcntl(output[0], F_SETFL, ::fcntl(output[0], F_GETFL) | O_NONBLOCK);

    const auto drain = [&] {
        char chunk[512];
        while (true) {
            const ssize_t count = ::read(output[0], chunk, sizeof(chunk));
            if (count > 0) {
                if (result.output.size() < maximum_output) result.output.append(chunk, static_cast<std::size_t>(count));
                continue;
            }
            if (count < 0 && errno == EINTR) continue;
            break;
        }
    };

    const auto deadline = std::chrono::steady_clock::now() + limit;
    int wait_status = 0;
    bool finished = false;
    while (true) {
        drain();
        const pid_t done = ::waitpid(child, &wait_status, WNOHANG);
        if (done == child || (done < 0 && errno != EINTR)) {
            finished = true;
            break;
        }
        if (std::chrono::steady_clock::now() >= deadline) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    if (!finished) {
        ::kill(child, SIGKILL);
        ::waitpid(child, &wait_status, 0);
        result.timed_out = true;
    } else {
        drain();
        if (WIFEXITED(wait_status)) result.exit_code = WEXITSTATUS(wait_status);
    }
    ::close(output[0]);
    result.output = trimmed(std::move(result.output));
    if (result.output.size() > maximum_output) result.output.resize(maximum_output);
    if (result.timed_out) result.output += result.output.empty() ? "(timed out)" : " (timed out)";
    return result;
}

}  // namespace frame_notify::system
