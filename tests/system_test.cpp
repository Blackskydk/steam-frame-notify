#include "system/autostart.h"
#include "system/process.h"
#include "system/single_instance.h"

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <chrono>
#include <sstream>
#include <string>

// The single-instance lock and the autostart manager, the latter against a fake `systemctl`.

namespace {

using namespace frame_notify::system;
namespace fs = std::filesystem;

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "system_test.cpp:" << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(condition) expect((condition), #condition, __LINE__)

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

void write_script(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
    ::chmod(path.c_str(), 0700);
}

}  // namespace

int main() {
    const fs::path work = fs::temp_directory_path() / ("frame-notify-system-test-" + std::to_string(::getpid()));
    fs::create_directories(work);

    // ---- Single instance ----
    {
        const fs::path directory = work / "runtime";
        fs::create_directories(directory);
        std::optional<SingleInstance> first(std::in_place);
        EXPECT(!first->held() && first->acquire(directory.string()) && first->held());
        EXPECT(first->acquire(directory.string()));                      // again: still ours
        EXPECT(read_file(directory / "frame-notify.lock") == std::to_string(::getpid()) + "\n");

        SingleInstance second;
        EXPECT(!second.acquire(directory.string()) && !second.held());
        EXPECT(second.holder() == ::getpid());
        EXPECT(contains(second.error(), "already running"));

        SingleInstance other_name;                                       // another lock file is independent
        EXPECT(other_name.acquire(directory.string(), "other.lock"));

        // A child started while the lock was held must not keep it: once the owner is gone a new
        // instance can start even though that child is still running.
        const fs::path started = work / "child-started";
        const std::string script = "echo up > '" + started.string() + "'; exec sleep 5";
        const pid_t child = ::fork();
        if (child == 0) {
            ::execl("/bin/sh", "sh", "-c", script.c_str(), static_cast<char*>(nullptr));
            ::_exit(127);
        }
        EXPECT(child > 0);
        for (int attempt = 0; attempt < 500 && !fs::exists(started); ++attempt) {
            ::usleep(10000);                                             // wait until the child has exec'ed
        }
        EXPECT(fs::exists(started));
        first.reset();                                                   // the lock goes with the object
        SingleInstance successor;
        EXPECT(successor.acquire(directory.string()) && successor.holder() == 0);
        EXPECT(read_file(directory / "frame-notify.lock") == std::to_string(::getpid()) + "\n");
        if (child > 0) {
            ::kill(child, SIGKILL);
            ::waitpid(child, nullptr, 0);
        }

        SingleInstance missing_directory;
        EXPECT(!missing_directory.acquire((work / "no-such-directory").string()));
        EXPECT(contains(missing_directory.error(), "cannot open"));
        SingleInstance nowhere;
        EXPECT(!nowhere.acquire("") && contains(nowhere.error(), "XDG_RUNTIME_DIR"));
    }

    // ---- Running programs ----
    {
        const auto ok = run_process({"/bin/sh", "-c", "echo out; echo err >&2; exit 3"});
        EXPECT(ok.started && ok.exit_code == 3 && !ok.timed_out);
        EXPECT(contains(ok.output, "out") && contains(ok.output, "err"));
        const auto plain = run_process({"/bin/sh", "-c", "exit 0"});
        EXPECT(plain.started && plain.exit_code == 0 && plain.output.empty());

        ::unsetenv("FN_PROCESS_TEST");
        EXPECT(run_process({"/bin/sh", "-c", "printf %s \"$FN_PROCESS_TEST\""}, {{"FN_PROCESS_TEST", "default"}}).output == "default");
        ::setenv("FN_PROCESS_TEST", "mine", 1);
        EXPECT(run_process({"/bin/sh", "-c", "printf %s \"$FN_PROCESS_TEST\""}, {{"FN_PROCESS_TEST", "default"}}).output == "mine");
        ::setenv("FN_PROCESS_TEST", "", 1);                               // empty counts as not set
        EXPECT(run_process({"/bin/sh", "-c", "printf %s \"$FN_PROCESS_TEST\""}, {{"FN_PROCESS_TEST", "default"}}).output == "default");
        ::unsetenv("FN_PROCESS_TEST");

        const auto missing = run_process({"/nonexistent/program-for-frame-notify"});
        EXPECT(!missing.started || missing.exit_code == 127);
        EXPECT(!run_process({}).started);

        const auto began = std::chrono::steady_clock::now();
        const auto slow = run_process({"/bin/sh", "-c", "echo before; exec sleep 30"}, {}, std::chrono::milliseconds(300));
        EXPECT(slow.started && slow.timed_out && slow.exit_code == -1);
        EXPECT(contains(slow.output, "before") && contains(slow.output, "timed out"));
        EXPECT(std::chrono::steady_clock::now() - began < std::chrono::seconds(5));

        const auto loud = run_process({"/bin/sh", "-c", "head -c 100000 /dev/zero | tr '\\0' x"}, {}, std::chrono::seconds(10), 1000);
        EXPECT(loud.exit_code == 0 && loud.output.size() <= 1000U && !loud.output.empty());
    }

    // ---- A program in the background ----
    {
        AsyncProcess idle;
        EXPECT(idle.poll() && !idle.running() && !idle.result().started);   // nothing started: nothing to wait for

        AsyncProcess process;
        EXPECT(process.start({"/bin/sh", "-c", "sleep 1; echo done; exit 4"}));
        EXPECT(process.running());
        const auto began = std::chrono::steady_clock::now();
        EXPECT(!process.poll());                                          // still going, and poll did not wait
        EXPECT(std::chrono::steady_clock::now() - began < std::chrono::milliseconds(300));
        while (!process.poll()) ::usleep(5000);
        EXPECT(!process.running() && process.result().started && process.result().exit_code == 4);
        EXPECT(process.result().output == "done" && !process.result().timed_out);
        EXPECT(process.poll());                                           // and stays finished

        EXPECT(process.start({"/bin/sh", "-c", "echo again"}));            // the same object can run another program
        EXPECT(process.result().output.empty());
        while (!process.poll()) ::usleep(5000);
        EXPECT(process.result().output == "again" && process.result().exit_code == 0);

        EXPECT(process.start({"/bin/sh", "-c", "exec sleep 30"}, {}, std::chrono::milliseconds(250)));
        const auto waiting = std::chrono::steady_clock::now();
        while (!process.poll()) ::usleep(5000);
        EXPECT(process.result().timed_out && process.result().exit_code == -1);
        EXPECT(std::chrono::steady_clock::now() - waiting < std::chrono::seconds(5));

        const fs::path marker = work / "async-ran";
        {
            AsyncProcess leaving;
            EXPECT(leaving.start({"/bin/sh", "-c", "sleep 1; echo late > '" + marker.string() + "'"}));
        }                                                                 // destroyed while running: the program is stopped
        ::usleep(1500000);
        EXPECT(!fs::exists(marker));

        AsyncProcess broken;
        const bool started = broken.start({"/nonexistent/program-for-frame-notify"});
        while (started && !broken.poll()) ::usleep(5000);
        EXPECT(!started ? (broken.poll() && !broken.result().started && !broken.result().output.empty())
                        : broken.result().exit_code == 127);
        EXPECT(!broken.start({}));
    }

    // ---- Autostart ----
    const fs::path bin = work / "bin";
    const fs::path config = work / "config";
    const fs::path fake_state = work / "systemd-state";
    fs::create_directories(bin);
    fs::create_directories(fake_state);
    const fs::path calls = fake_state / "calls.log";
    // A stand-in for systemctl: logs each call and the environment it was given, keeps an "enabled"
    // marker, and fails on demand (FAKE_FAIL=<subcommand>).
    write_script(bin / "systemctl",
                 "#!/bin/sh\n"
                 "echo \"$* | XDG_RUNTIME_DIR=$XDG_RUNTIME_DIR BUS=$DBUS_SESSION_BUS_ADDRESS\" >> '" + calls.string() + "'\n"
                 "command=$2\n"
                 "if [ \"$FAKE_FAIL\" = \"$command\" ]; then echo \"Failed to $command: boom\" >&2; exit 1; fi\n"
                 "case \"$command\" in\n"
                 "  enable) touch '" + (fake_state / "enabled").string() + "';;\n"
                 "  disable) rm -f '" + (fake_state / "enabled").string() + "';;\n"
                 "  is-enabled) [ -f '" + (fake_state / "enabled").string() + "' ] || { echo disabled; exit 1; };;\n"
                 "esac\n"
                 "exit 0\n");
    ::unsetenv("FAKE_FAIL");

    Autostart::Options options;
    options.executable = "/home/steamos/.local/share/frame-notify/frame-notify";
    options.config_home = config.string();
    options.systemctl = (bin / "systemctl").string();
    options.runtime_dir = (work / "run-user").string();

    // The files it writes.
    {
        const std::string unit = Autostart::unit_text("/opt/my apps/frame notify/frame-notify");
        EXPECT(contains(unit, "ExecStart=\"/opt/my apps/frame notify/frame-notify\"\n"));
        EXPECT(contains(unit, "WantedBy=default.target") && contains(unit, "Restart=on-failure"));
        EXPECT(contains(unit, "[Unit]") && contains(unit, "[Service]") && contains(unit, "[Install]"));
        EXPECT(Autostart::systemd_quote("a\"b\\c%d$e") == "\"a\\\"b\\\\c%%d$$e\"");
        const std::string entry = Autostart::desktop_entry_text("/home/x y/$frame`notify");
        EXPECT(contains(entry, "Exec=\"/home/x y/\\$frame\\`notify\"\n") && contains(entry, "Type=Application"));
    }

    {
        Autostart autostart(options);
        EXPECT(!autostart.status().enabled && autostart.status().method == AutostartMethod::kNone);
        EXPECT(autostart.unit_path() == (config / "systemd/user/frame-notify.service").string());

        std::string error;
        EXPECT(autostart.enable(error) && error.empty());
        EXPECT(read_file(autostart.unit_path()) == Autostart::unit_text(options.executable));
        const std::string log = read_file(calls);
        EXPECT(contains(log, "--user daemon-reload") && contains(log, "--user enable frame-notify.service"));
        // systemctl is given the user's runtime dir and bus even when the shell had none.
        EXPECT(contains(log, "XDG_RUNTIME_DIR=" + (work / "run-user").string()) ||
               std::getenv("XDG_RUNTIME_DIR") != nullptr);
        EXPECT(log.find("daemon-reload") < log.find("enable"));
        const AutostartStatus status = autostart.status();
        EXPECT(status.enabled && status.method == AutostartMethod::kSystemd && status.detail.empty());
        EXPECT(!fs::exists(autostart.desktop_entry_path()));

        EXPECT(autostart.enable(error));                                 // enabling twice is harmless
        EXPECT(autostart.status().enabled);

        // The file is there but systemd says it is not enabled.
        fs::remove(fake_state / "enabled");
        EXPECT(!autostart.status().enabled && autostart.status().method == AutostartMethod::kSystemd &&
               !autostart.status().detail.empty());
        EXPECT(autostart.enable(error) && autostart.status().enabled);

        EXPECT(autostart.disable(error) && error.empty());
        EXPECT(!fs::exists(autostart.unit_path()) && !fs::exists(fake_state / "enabled"));
        EXPECT(!autostart.status().enabled && autostart.status().method == AutostartMethod::kNone);
        EXPECT(contains(read_file(calls), "--user disable frame-notify.service"));
        EXPECT(autostart.disable(error) && error.empty());               // nothing to do is fine
    }

    // A path that changed (the program was moved or updated) is fixed by enabling again.
    {
        Autostart autostart(options);
        std::string error;
        EXPECT(autostart.enable(error));
        Autostart::Options moved = options;
        moved.executable = "/home/steamos/elsewhere/frame-notify";
        Autostart relocated(moved);
        EXPECT(relocated.enable(error));
        EXPECT(contains(read_file(autostart.unit_path()), "/home/steamos/elsewhere/frame-notify"));
        EXPECT(!contains(read_file(autostart.unit_path()), ".local/share"));
        EXPECT(relocated.disable(error));
    }

    // systemd refuses: nothing half-done is left, and a desktop autostart entry takes over.
    for (const char* failing : {"daemon-reload", "enable"}) {
        ::setenv("FAKE_FAIL", failing, 1);
        Autostart autostart(options);
        std::string error;
        EXPECT(autostart.enable(error) && error.empty());
        EXPECT(!fs::exists(autostart.unit_path()));                      // the service file was withdrawn
        EXPECT(fs::exists(autostart.desktop_entry_path()));
        EXPECT(contains(read_file(autostart.desktop_entry_path()), "Exec=\"" + options.executable + "\""));
        const AutostartStatus status = autostart.status();
        EXPECT(status.enabled && status.method == AutostartMethod::kDesktopEntry);
        ::unsetenv("FAKE_FAIL");

        EXPECT(autostart.enable(error));                                 // systemd works again: it wins
        EXPECT(fs::exists(autostart.unit_path()) && !fs::exists(autostart.desktop_entry_path()));
        EXPECT(autostart.status().method == AutostartMethod::kSystemd);
        EXPECT(autostart.disable(error));
        EXPECT(!fs::exists(autostart.unit_path()) && !fs::exists(autostart.desktop_entry_path()));
    }

    // No systemctl at all.
    {
        Autostart::Options without = options;
        without.systemctl = (work / "no-such-systemctl").string();
        Autostart autostart(without);
        std::string error;
        EXPECT(autostart.enable(error));
        EXPECT(!fs::exists(autostart.unit_path()) && fs::exists(autostart.desktop_entry_path()));
        EXPECT(autostart.status().enabled && autostart.status().method == AutostartMethod::kDesktopEntry);
        EXPECT(autostart.disable(error) && !autostart.status().enabled);
    }

    // The configuration directory cannot be written: both ways fail and the error says so.
    {
        Autostart::Options blocked = options;
        const fs::path file = work / "a-file";
        std::ofstream(file) << "x";
        blocked.config_home = (file / "config").string();                // a directory under a regular file
        Autostart autostart(blocked);
        std::string error;
        EXPECT(!autostart.enable(error) && contains(error, "systemd:") && contains(error, "autostart entry:"));
        EXPECT(!autostart.status().enabled);
    }

    // Without a program or a configuration directory there is nothing to set up.
    {
        Autostart::Options empty;
        Autostart autostart(empty);
        std::string error;
        EXPECT(!autostart.enable(error) && !error.empty());
    }

    // The defaults point at this program and the user's configuration directory.
    {
        ::setenv("XDG_CONFIG_HOME", "/tmp/custom-config", 1);
        EXPECT(Autostart::default_options().config_home == "/tmp/custom-config");
        ::unsetenv("XDG_CONFIG_HOME");
        ::setenv("HOME", "/tmp/custom-home", 1);
        EXPECT(Autostart::default_options().config_home == "/tmp/custom-home/.config");
        EXPECT(!Autostart::default_options().executable.empty());
    }

    std::error_code ignored;
    fs::remove_all(work, ignored);

    if (failures != 0) {
        std::cerr << failures << " expectation(s) failed\n";
        return 1;
    }
    return 0;
}
