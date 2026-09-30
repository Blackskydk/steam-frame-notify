#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frame_notify::bluetooth {

// What the helper last said about the phone. `state` and `fields` come straight from its "state"
// events (see scripts/ancs_service.py). Two more states are added here for the helper process
// itself: "helper_restarting" while it is being started again after a crash, and
// "helper_unavailable" when it cannot run (fields: "message").
struct PhoneStatus {
    std::string state = "starting";
    std::map<std::string, std::string> fields;
    std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();

    [[nodiscard]] std::string field(const std::string& name, const std::string& fallback = {}) const;
};

// Runs the Bluetooth helper (scripts/ancs_bridge.py --service) as a child process and talks to it
// with one JSON object per line: its events arrive on the child's stdout, commands go to its stdin.
// Nothing here blocks: call poll() regularly from the main loop. A helper that crashes is started
// again with a growing pause; one that reports a fatal problem (for example missing Python
// modules) is not, until retry() is called.
class PhoneLink {
public:
    PhoneLink() = default;
    ~PhoneLink();

    PhoneLink(const PhoneLink&) = delete;
    PhoneLink& operator=(const PhoneLink&) = delete;

    // Starts the helper: `command` is the program followed by its arguments.
    bool start(std::vector<std::string> command);
    // Reads what the helper has said, notices when it has exited and restarts it when due.
    // Returns true when status() changed since the previous call.
    bool poll();
    // Sends {"command":<command>, ...fields}; false when the helper is not running.
    bool send(std::string_view command,
              const std::vector<std::pair<std::string, std::string>>& fields = {});
    // Asks a running helper to check Bluetooth again, or starts a stopped one immediately.
    void retry();
    // Asks the helper to quit and waits a moment, then terminates it.
    void stop();

    [[nodiscard]] const PhoneStatus& status() const noexcept { return status_; }
    [[nodiscard]] bool running() const noexcept { return pid_ > 0; }

    // Tuning, mainly for tests.
    void set_restart_backoff(std::chrono::milliseconds first, std::chrono::milliseconds maximum) {
        backoff_first_ = first;
        backoff_maximum_ = maximum;
    }
    void set_stop_timeouts(std::chrono::milliseconds quit, std::chrono::milliseconds terminate) {
        quit_timeout_ = quit;
        terminate_timeout_ = terminate;
    }

private:
    bool spawn();
    void read_output();
    void process_lines();
    bool reap();
    void close_pipes() noexcept;
    void set_state(std::string state, std::map<std::string, std::string> fields);
    void give_up(const std::string& message);

    std::vector<std::string> command_;
    PhoneStatus status_;
    bool status_changed_ = false;
    int pid_ = 0;
    int to_child_ = -1;
    int from_child_ = -1;
    std::string buffer_;
    std::string fatal_message_;
    std::chrono::steady_clock::time_point started_at_;
    bool restart_due_ = false;
    std::chrono::steady_clock::time_point restart_at_;
    int quick_failures_ = 0;
    bool stopped_ = false;
    std::chrono::milliseconds backoff_first_{2000};
    std::chrono::milliseconds backoff_maximum_{30000};
    std::chrono::milliseconds quit_timeout_{2000};
    std::chrono::milliseconds terminate_timeout_{1000};
};

// The `ids` field of the helper's "clear_notifications" command: the ids in `ids` that the helper
// made (they start with "ancs-"), joined with commas, at most 200 of them. Empty when none is.
[[nodiscard]] std::string clear_notifications_field(const std::vector<std::string>& ids);

// The script that implements the helper, found next to the executable or in the source tree; empty
// when there is none. FRAME_NOTIFY_BRIDGE overrides the search with an explicit path.
[[nodiscard]] std::string find_helper_script();
// python3 <script> --service, or an empty command when the script cannot be found.
// FRAME_NOTIFY_PYTHON overrides the interpreter.
[[nodiscard]] std::vector<std::string> default_helper_command();

}  // namespace frame_notify::bluetooth
