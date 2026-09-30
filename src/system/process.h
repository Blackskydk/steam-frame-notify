#pragma once

#include <chrono>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace frame_notify::system {

struct ProcessResult {
    bool started = false;     // the program could be run at all
    int exit_code = -1;       // -1 when it did not exit normally (killed, or timed out)
    bool timed_out = false;
    std::string output;       // what it printed (stdout and stderr together), trimmed and capped
};

// A program running in the background while the caller goes on with other things. Start it, call
// poll() now and then, and read result() once poll() says it is over. Destroying the object while
// the program still runs stops the program.
class AsyncProcess {
public:
    AsyncProcess() = default;
    ~AsyncProcess();

    AsyncProcess(const AsyncProcess&) = delete;
    AsyncProcess& operator=(const AsyncProcess&) = delete;

    // Starts `command` (program first, found on the PATH). The environment is this process's, plus
    // each of `default_environment` that is not already set to something. It is stopped after
    // `limit`. False when it could not be started; result() then says why and poll() returns true.
    bool start(const std::vector<std::string>& command,
               const std::vector<std::pair<std::string, std::string>>& default_environment = {},
               std::chrono::milliseconds limit = std::chrono::seconds(20),
               std::size_t maximum_output = 4096);
    // Collects what the program printed and notices that it ended or ran out of time. Returns true
    // once it is over (also when nothing was started).
    [[nodiscard]] bool poll();
    [[nodiscard]] bool running() const noexcept { return pid_ > 0; }
    [[nodiscard]] const ProcessResult& result() const noexcept { return result_; }

private:
    void drain();
    void stop_program();

    int pid_ = 0;
    int output_ = -1;
    std::chrono::steady_clock::time_point deadline_;
    std::size_t maximum_output_ = 0;
    ProcessResult result_;
};

// Runs a program and waits for it, for at most `limit`, collecting what it prints.
[[nodiscard]] ProcessResult run_process(
    const std::vector<std::string>& command,
    const std::vector<std::pair<std::string, std::string>>& default_environment = {},
    std::chrono::milliseconds limit = std::chrono::seconds(20), std::size_t maximum_output = 4096);

}  // namespace frame_notify::system
