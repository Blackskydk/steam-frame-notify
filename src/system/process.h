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

// Runs a program and waits for it, for at most `limit`, collecting what it prints. The environment
// is this process's, plus each of `default_environment` that is not already set to something.
[[nodiscard]] ProcessResult run_process(
    const std::vector<std::string>& command,
    const std::vector<std::pair<std::string, std::string>>& default_environment = {},
    std::chrono::milliseconds limit = std::chrono::seconds(20), std::size_t maximum_output = 4096);

}  // namespace frame_notify::system
