#include "bluetooth/phone_link.h"

#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

// Runs real child processes (small `sh` scripts standing in for the Bluetooth helper) and checks
// how PhoneLink reads their events, sends commands, restarts them and stops them.

namespace {

using frame_notify::bluetooth::PhoneLink;
using frame_notify::bluetooth::PhoneStatus;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "phone_link_test.cpp:" << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(condition) expect((condition), #condition, __LINE__)

std::vector<std::string> script(const std::string& body) {
    return {"sh", "-c", body};
}

// Polls until `done` holds or the time is up.
bool wait_until(PhoneLink& link, const std::function<bool()>& done,
                std::chrono::milliseconds limit = 8000ms) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (true) {
        link.poll();
        if (done()) return true;
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(5ms);
    }
}

bool wait_for_state(PhoneLink& link, const std::string& state,
                    std::chrono::milliseconds limit = 8000ms) {
    return wait_until(link, [&] { return link.status().state == state; }, limit);
}

// Keeps polling for a while, for checking that something does NOT happen.
void poll_for(PhoneLink& link, std::chrono::milliseconds duration) {
    const auto deadline = std::chrono::steady_clock::now() + duration;
    while (std::chrono::steady_clock::now() < deadline) {
        link.poll();
        std::this_thread::sleep_for(5ms);
    }
}

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

int count_lines(const fs::path& path) {
    std::istringstream stream(read_file(path));
    int count = 0;
    std::string line;
    while (std::getline(stream, line)) ++count;
    return count;
}

// A `sh` string literal for a path.
std::string quoted(const fs::path& path) {
    return "'" + path.string() + "'";
}

void quick(PhoneLink& link) {
    link.set_restart_backoff(10ms, 20ms);
    link.set_stop_timeouts(300ms, 300ms);
}

}  // namespace

int main() {
    const fs::path work = fs::temp_directory_path() / ("frame-notify-phone-link-test-" + std::to_string(::getpid()));
    fs::create_directories(work);

    // ---- Events from the helper become the status ----
    {
        PhoneLink link;
        quick(link);
        EXPECT(link.status().state == "starting" && !link.running());
        EXPECT(link.start(script(R"(printf '{"event":"hello","version":"1"}\n{"event":"state","state":"unpaired"}\n'; exec sleep 30)")));
        EXPECT(link.running());
        EXPECT(wait_for_state(link, "unpaired"));
        EXPECT(link.status().fields.empty());
        EXPECT(!link.poll());                                                // nothing new: no change reported
        link.stop();
        EXPECT(!link.running());
    }
    {
        PhoneLink link;
        quick(link);
        EXPECT(link.start(script(R"(printf '{"event":"state","state":"pair_confirm","code":"628640","phone":"iPhone","seconds":"40"}\n'; exec sleep 30)")));
        EXPECT(wait_for_state(link, "pair_confirm"));
        EXPECT(link.status().field("code") == "628640" && link.status().field("phone") == "iPhone");
        EXPECT(link.status().field("seconds") == "40");
        EXPECT(link.status().fields.count("event") == 0U && link.status().fields.count("state") == 0U);
        EXPECT(link.status().field("missing", "fallback") == "fallback" && link.status().field("missing").empty());
        EXPECT(link.status().fields.size() == 3U);
    }
    {   // A change of fields alone is a change; repeating the same state is not.
        PhoneLink link;
        quick(link);
        EXPECT(link.start(script(R"(read go
printf '{"event":"state","state":"connecting","phone":"A"}\n{"event":"state","state":"connecting","phone":"A"}\n'
read go
printf '{"event":"state","state":"connecting","phone":"B"}\n'
exec sleep 30)")));
        EXPECT(link.send("go"));
        EXPECT(wait_until(link, [&] { return link.status().field("phone") == "A"; }));
        EXPECT(!link.poll());
        EXPECT(link.send("go"));
        EXPECT(wait_until(link, [&] { return link.status().field("phone") == "B"; }));
    }
    {   // Noise: events that are not states, bad JSON, blank lines, CRLF, a line split across reads.
        PhoneLink link;
        quick(link);
        EXPECT(link.start(script(R"(printf 'not json at all\n\n{"event":"mystery","state":"connected"}\n{"event":"state"}\n[1,2]\n{"event":"state","state":"con'
read go
printf 'nected","phone":"iPhone"}\r\n'
exec sleep 30)")));
        poll_for(link, 200ms);
        EXPECT(link.status().state == "starting");                           // nothing above was a state
        EXPECT(link.send("go"));
        EXPECT(wait_for_state(link, "connected"));
        EXPECT(link.status().field("phone") == "iPhone");
    }
    {   // A very long line without an end is dropped rather than buffered forever.
        PhoneLink link;
        quick(link);
        EXPECT(link.start(script(R"(head -c 1300000 /dev/zero | tr '\0' 'a'
read go
printf '\n{"event":"state","state":"connected"}\n'
exec sleep 30)")));
        poll_for(link, 400ms);
        EXPECT(link.send("go"));
        EXPECT(wait_for_state(link, "connected"));
    }
    {   // A burst of events: the last one wins.
        PhoneLink link;
        quick(link);
        EXPECT(link.start(script(R"(i=0
while [ $i -lt 3000 ]; do printf '{"event":"state","state":"connecting","n":"%s"}\n' $i; i=$((i+1)); done
printf '{"event":"state","state":"connected","n":"last"}\n'
exec sleep 30)")));
        EXPECT(wait_for_state(link, "connected"));
        EXPECT(link.status().field("n") == "last");
    }

    // ---- Commands reach the helper, one JSON object per line ----
    {
        const fs::path log = work / "commands.log";
        PhoneLink link;
        quick(link);
        EXPECT(!link.send("pair"));                                          // nothing is running yet
        EXPECT(link.start(script("exec cat > " + quoted(log))));
        EXPECT(link.send("pair"));
        EXPECT(link.send("remove_conflict", {{"address", "AA:BB:CC:DD:EE:01"}}));
        EXPECT(link.send("say", {{"text", "quote \" backslash \\ newline \n tab \t"}}));
        EXPECT(link.send("retry"));
        link.stop();                                                         // says "quit", then closes its input
        EXPECT(!link.running() && !link.send("pair"));
        const std::string written = read_file(log);
        EXPECT(written.find("{\"command\":\"pair\"}\n") == 0U);
        EXPECT(written.find("{\"command\":\"remove_conflict\",\"address\":\"AA:BB:CC:DD:EE:01\"}\n") != std::string::npos);
        EXPECT(written.find("{\"command\":\"say\",\"text\":\"quote \\\" backslash \\\\ newline \\n tab \\t\"}\n") != std::string::npos);
        const std::string farewell = "{\"command\":\"retry\"}\n{\"command\":\"quit\"}\n";
        EXPECT(written.size() > farewell.size() &&
               written.compare(written.size() - farewell.size(), farewell.size(), farewell) == 0);
        EXPECT(count_lines(log) == 5);
    }
    {   // retry() on a running helper is just a command.
        const fs::path log = work / "retry.log";
        PhoneLink link;
        quick(link);
        EXPECT(link.start(script("exec cat > " + quoted(log))));
        link.retry();
        link.stop();
        EXPECT(read_file(log) == "{\"command\":\"retry\"}\n{\"command\":\"quit\"}\n");
    }

    // ---- Clearing on the iPhone: only the helper's own ids are passed on ----
    {
        using frame_notify::bluetooth::clear_notifications_field;
        EXPECT(clear_notifications_field({}).empty());
        EXPECT(clear_notifications_field({"a-1", "manual", "", "ancs"}).empty());
        EXPECT(clear_notifications_field({"ancs-0123456789abcdef01234567"}) == "ancs-0123456789abcdef01234567");
        EXPECT(clear_notifications_field({"ancs-a", "other-1", "ancs-b,ancs-c", "ancs-d"}) == "ancs-a,ancs-d");
        EXPECT(clear_notifications_field({"ancs-"}) == "ancs-");               // the helper decides what is valid
        std::vector<std::string> many;
        for (int number = 0; number < 500; ++number) many.push_back("ancs-" + std::to_string(number));
        const std::string capped = clear_notifications_field(many);
        EXPECT(std::count(capped.begin(), capped.end(), ',') == 199);
        EXPECT(capped.rfind("ancs-0,ancs-1,", 0) == 0U && capped.substr(capped.rfind(',') + 1) == "ancs-199");

        const fs::path log = work / "clear.log";
        PhoneLink link;
        quick(link);
        EXPECT(link.start(script("exec cat > " + quoted(log))));
        EXPECT(link.send("clear_notifications", {{"ids", clear_notifications_field({"ancs-a", "ancs-b"})}}));
        link.stop();
        EXPECT(read_file(log) == "{\"command\":\"clear_notifications\",\"ids\":\"ancs-a,ancs-b\"}\n"
                                 "{\"command\":\"quit\"}\n");
    }

    // ---- A helper that crashes is started again, a few times ----
    {
        const fs::path runs = work / "crash-runs.log";
        PhoneLink link;
        quick(link);
        EXPECT(link.start(script("echo run >> " + quoted(runs) + "; exit 3")));
        EXPECT(wait_for_state(link, "helper_restarting"));
        EXPECT(link.status().field("reason") == "exit code 3");
        EXPECT(wait_until(link, [&] { return count_lines(runs) >= 2; }));    // and it did start again
        EXPECT(wait_for_state(link, "helper_unavailable"));                  // until it was too often
        EXPECT(link.status().field("message").find("keeps stopping") != std::string::npos);
        EXPECT(link.status().field("message").find("exit code 3") != std::string::npos);
        EXPECT(!link.running());
        const int runs_at_giving_up = count_lines(runs);
        EXPECT(runs_at_giving_up == 6);                                      // the first run and five more
        poll_for(link, 150ms);
        EXPECT(count_lines(runs) == runs_at_giving_up && !link.running());
        link.retry();                                                        // the user asks again
        EXPECT(wait_until(link, [&] { return count_lines(runs) == runs_at_giving_up + 1; }));
        link.stop();
    }
    {   // A crash by signal is described as one.
        PhoneLink link;
        quick(link);
        EXPECT(link.start(script("kill -9 $$")));
        EXPECT(wait_for_state(link, "helper_restarting"));
        EXPECT(link.status().field("reason") == "signal 9");
        link.stop();
    }
    {   // The restart waits; nothing is started before its time.
        const fs::path runs = work / "backoff-runs.log";
        PhoneLink link;
        link.set_restart_backoff(400ms, 400ms);
        link.set_stop_timeouts(300ms, 300ms);
        EXPECT(link.start(script("echo run >> " + quoted(runs) + "; exit 1")));
        EXPECT(wait_for_state(link, "helper_restarting"));
        poll_for(link, 150ms);
        EXPECT(count_lines(runs) == 1 && link.status().state == "helper_restarting");
        EXPECT(wait_until(link, [&] { return count_lines(runs) == 2; }));
        link.stop();
        poll_for(link, 500ms);                                               // stopped: it stays stopped
        EXPECT(count_lines(runs) == 2 && !link.running());
    }
    {   // The pause between restarts doubles, up to its maximum.
        const fs::path runs = work / "capped-runs.log";
        PhoneLink link;
        link.set_restart_backoff(20ms, 40ms);
        link.set_stop_timeouts(300ms, 300ms);
        EXPECT(link.start(script("echo run >> " + quoted(runs) + "; exit 1")));
        const auto began = std::chrono::steady_clock::now();
        EXPECT(wait_for_state(link, "helper_unavailable"));
        // Pauses of 20, 40, 40, 40, 40 ms: well under a second even with slow process start-up.
        EXPECT(std::chrono::steady_clock::now() - began < 6s);
        EXPECT(count_lines(runs) == 6);
    }

    // ---- A fatal report is final until someone retries ----
    {
        const fs::path runs = work / "fatal-runs.log";
        PhoneLink link;
        quick(link);
        EXPECT(link.start(script("echo run >> " + quoted(runs) +
                                 R"(; printf '{"event":"fatal","message":"No module named dbus"}\n'; exit 1)")));
        EXPECT(wait_for_state(link, "helper_unavailable"));
        EXPECT(link.status().field("message") == "No module named dbus");
        EXPECT(!link.running());
        poll_for(link, 200ms);
        EXPECT(count_lines(runs) == 1 && link.status().state == "helper_unavailable");   // no restart loop
        link.retry();
        EXPECT(wait_until(link, [&] { return count_lines(runs) == 2; }));
        EXPECT(wait_for_state(link, "helper_unavailable"));
        link.stop();
    }
    {   // A fatal event without a message still says something.
        PhoneLink link;
        quick(link);
        EXPECT(link.start(script(R"(printf '{"event":"fatal"}\n'; exit 1)")));
        EXPECT(wait_for_state(link, "helper_unavailable"));
        EXPECT(!link.status().field("message").empty());
    }

    // ---- Cannot start at all ----
    {
        PhoneLink link;
        quick(link);
        EXPECT(!link.start({}));
        EXPECT(link.status().state == "helper_unavailable" && !link.status().field("message").empty());
        EXPECT(!link.running() && !link.send("pair"));
        link.retry();                                                        // nothing to retry
        poll_for(link, 50ms);
        EXPECT(link.status().state == "helper_unavailable");
    }
    {
        PhoneLink link;
        quick(link);
        link.start({"/nonexistent/frame-notify-helper", "--service"});
        EXPECT(wait_for_state(link, "helper_unavailable"));
        EXPECT(!link.running());
    }

    // ---- Starting again replaces the helper ----
    {
        PhoneLink link;
        quick(link);
        EXPECT(link.start(script(R"(printf '{"event":"state","state":"unpaired"}\n'; exec sleep 30)")));
        EXPECT(wait_for_state(link, "unpaired"));
        EXPECT(link.start(script(R"(printf '{"event":"state","state":"connected"}\n'; exec sleep 30)")));
        EXPECT(link.running());
        EXPECT(wait_for_state(link, "connected"));
        poll_for(link, 100ms);
        EXPECT(link.status().state == "connected" && link.running());
    }

    // ---- Stopping ----
    {   // A helper that leaves when its input closes is gone at once.
        PhoneLink link;
        link.set_stop_timeouts(3000ms, 3000ms);
        EXPECT(link.start(script("exec cat > /dev/null")));
        const auto began = std::chrono::steady_clock::now();
        link.stop();
        EXPECT(std::chrono::steady_clock::now() - began < 1500ms && !link.running());
        link.stop();                                                         // harmless twice
    }
    {   // One that ignores that is asked to quit, terminated, and killed in the end.
        PhoneLink link;
        link.set_stop_timeouts(50ms, 50ms);
        EXPECT(link.start(script("trap '' TERM HUP; while :; do sleep 1; done")));
        std::this_thread::sleep_for(150ms);                                  // let it install the trap
        const auto began = std::chrono::steady_clock::now();
        link.stop();
        EXPECT(std::chrono::steady_clock::now() - began < 4s && !link.running());
    }
    {   // Leaving scope stops the helper too.
        const fs::path pid_file = work / "child.pid";
        {
            PhoneLink link;
            link.set_stop_timeouts(50ms, 500ms);
            EXPECT(link.start(script("echo $$ > " + quoted(pid_file) + "; exec sleep 60")));
            EXPECT(wait_until(link, [&] { return !read_file(pid_file).empty(); }));
        }
        const int pid = std::atoi(read_file(pid_file).c_str());
        EXPECT(pid > 1);
        bool gone = false;
        for (int attempt = 0; attempt < 100 && !gone; ++attempt) {
            gone = ::kill(pid, 0) != 0;
            if (!gone) std::this_thread::sleep_for(20ms);
        }
        EXPECT(gone);
    }

    // ---- Finding the helper ----
    {
        const fs::path fake = work / "ancs_bridge.py";
        std::ofstream(fake) << "# not really a script\n";
        ::setenv("FRAME_NOTIFY_BRIDGE", fake.c_str(), 1);
        EXPECT(frame_notify::bluetooth::find_helper_script() == fake.string());
        ::unsetenv("FRAME_NOTIFY_PYTHON");
        auto command = frame_notify::bluetooth::default_helper_command();
        EXPECT(command.size() == 3U && command[0] == "python3" && command[1] == fake.string() && command[2] == "--service");
        ::setenv("FRAME_NOTIFY_PYTHON", "/opt/python/bin/python", 1);
        command = frame_notify::bluetooth::default_helper_command();
        EXPECT(command.size() == 3U && command[0] == "/opt/python/bin/python");
        ::setenv("FRAME_NOTIFY_PYTHON", "", 1);                               // empty means "use the default"
        EXPECT(frame_notify::bluetooth::default_helper_command()[0] == "python3");
        ::unsetenv("FRAME_NOTIFY_PYTHON");

        ::setenv("FRAME_NOTIFY_BRIDGE", (work / "nope.py").c_str(), 1);       // explicit but missing: no guessing
        EXPECT(frame_notify::bluetooth::find_helper_script().empty());
        EXPECT(frame_notify::bluetooth::default_helper_command().empty());

        ::unsetenv("FRAME_NOTIFY_BRIDGE");
        const std::string found = frame_notify::bluetooth::find_helper_script();
        EXPECT(found.empty() || (fs::exists(found) && fs::path(found).filename() == "ancs_bridge.py"));
    }

    std::error_code ignored;
    fs::remove_all(work, ignored);

    if (failures != 0) {
        std::cerr << failures << " expectation(s) failed\n";
        return 1;
    }
    return 0;
}
