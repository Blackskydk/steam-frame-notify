#include "system/update_install.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

// Updating in place: which copy may do it, the command it runs, and what each way of ending means,
// against a fake install.sh.

namespace {

using namespace frame_notify::system;
namespace fs = std::filesystem;

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "update_install_test.cpp:" << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(condition) expect((condition), #condition, __LINE__)

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void write_file(const fs::path& path, const std::string& text, mode_t mode = 0600) {
    std::ofstream(path, std::ios::binary) << text;
    ::chmod(path.c_str(), mode);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

bool finish(UpdateInstaller& installer, std::chrono::milliseconds limit = std::chrono::seconds(8)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (installer.poll()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// What the fake installer does is chosen by $FAKE_MODE; it writes its arguments to $FAKE_ARGS.
constexpr char kFakeInstaller[] = R"(#!/bin/sh
echo "$@" > "$FAKE_ARGS"
case "$FAKE_MODE" in
ok) echo "Downloading Frame Notify"; echo "Updated Frame Notify from 0.1.1 to 0.2.0"; exit 0 ;;
offline) echo "Downloading Frame Notify"; echo "error: could not download https://example.invalid/x.tar.gz" >&2; exit 1 ;;
checksum) echo "error: the download does not match its checksum" >&2; exit 1 ;;
other) echo "error: tar is needed" >&2; exit 1 ;;
two) echo "error: first thing" >&2; echo "warning: noise"; echo "error: the real reason" >&2; exit 1 ;;
long) printf 'error: %0300d\n' 7 >&2; exit 1 ;;
silent) echo "no marker here"; exit 3 ;;
slow) sleep 5; exit 0 ;;
esac
exit 9
)";

}  // namespace

int main() {
    const fs::path work = fs::temp_directory_path() / ("frame-notify-install-test-" + std::to_string(::getpid()));
    fs::create_directories(work);

    // ---- Which copy may update itself ----
    {
        const fs::path app = work / "app";
        const fs::path other = work / "unpacked";
        fs::create_directories(app);
        fs::create_directories(other);
        write_file(app / "install.sh", "#!/bin/sh\n", 0700);
        write_file(other / "install.sh", "#!/bin/sh\n", 0700);
        const std::string script = (app / "install.sh").string();
        EXPECT(UpdateInstaller::find_script((app / "frame-notify").string(), app.string()) == script);
        // The same folder by another name (a trailing slash, a detour through ..) is the same folder.
        EXPECT(UpdateInstaller::find_script((app / "frame-notify").string(), app.string() + "/") == script);
        EXPECT(UpdateInstaller::find_script((app / "frame-notify").string(), (other / ".." / "app").string()) == script);
        EXPECT(UpdateInstaller::find_script((other / "frame-notify").string(), app.string()).empty());   // another copy
        EXPECT(UpdateInstaller::find_script("frame-notify", app.string()).empty());
        EXPECT(UpdateInstaller::find_script("", app.string()).empty());
        EXPECT(UpdateInstaller::find_script((app / "frame-notify").string(), "").empty());
        EXPECT(UpdateInstaller::find_script((app / "frame-notify").string(), (work / "missing").string()).empty());
        fs::remove(app / "install.sh");
        EXPECT(UpdateInstaller::find_script((app / "frame-notify").string(), app.string()).empty());    // no installer there
        fs::create_directories(app / "install.sh");                                                     // a folder is not one
        EXPECT(UpdateInstaller::find_script((app / "frame-notify").string(), app.string()).empty());
    }
    {
        ::setenv("HOME", "/home/someone", 1);
        ::unsetenv("FRAME_NOTIFY_APP_DIR");
        EXPECT(UpdateInstaller::default_app_dir() == "/home/someone/.local/share/frame-notify");
        ::setenv("FRAME_NOTIFY_APP_DIR", "/opt/frame-notify", 1);
        EXPECT(UpdateInstaller::default_app_dir() == "/opt/frame-notify");
        ::setenv("FRAME_NOTIFY_APP_DIR", "", 1);
        EXPECT(UpdateInstaller::default_app_dir() == "/home/someone/.local/share/frame-notify");
        ::unsetenv("FRAME_NOTIFY_APP_DIR");
        ::unsetenv("HOME");
        EXPECT(UpdateInstaller::default_app_dir().empty());
    }

    // ---- Version names ----
    EXPECT(UpdateInstaller::valid_version("0.1.4") && UpdateInstaller::valid_version("10.20.30"));
    EXPECT(UpdateInstaller::valid_version("1.0.0-rc1") && UpdateInstaller::valid_version("2_1"));
    EXPECT(!UpdateInstaller::valid_version("") && !UpdateInstaller::valid_version("v0.1.4"));
    EXPECT(!UpdateInstaller::valid_version("-1.0") && !UpdateInstaller::valid_version(".1"));
    EXPECT(!UpdateInstaller::valid_version("1.0;reboot") && !UpdateInstaller::valid_version("1.0 2.0"));
    EXPECT(!UpdateInstaller::valid_version("../1.0") && !UpdateInstaller::valid_version("1.0/x"));
    EXPECT(!UpdateInstaller::valid_version("1.0\n2") && !UpdateInstaller::valid_version("$(id)"));
    EXPECT(!UpdateInstaller::valid_version(std::string(33, '1')) && UpdateInstaller::valid_version(std::string(32, '1')));

    // ---- Running the installer ----
    const fs::path script = work / "install.sh";
    const fs::path args = work / "args.txt";
    write_file(script, kFakeInstaller, 0700);
    ::setenv("FAKE_ARGS", args.c_str(), 1);
    const auto installer_for = [&](std::chrono::milliseconds limit = std::chrono::seconds(10)) {
        UpdateInstaller::Options options;
        options.script = script.string();
        options.bash = "sh";
        options.limit = limit;
        return UpdateInstaller(options);
    };

    {   // A good update.
        ::setenv("FAKE_MODE", "ok", 1);
        UpdateInstaller installer = installer_for();
        EXPECT(installer.available() && installer.status().state == InstallState::kIdle);
        EXPECT(!installer.poll());                                              // nothing started: nothing to report
        EXPECT(installer.start("0.2.0", true));
        EXPECT(installer.status().state == InstallState::kInstalling);
        EXPECT(!installer.start("0.2.0", true));                                // one at a time
        EXPECT(finish(installer));
        EXPECT(installer.status().state == InstallState::kDone && installer.status().message.empty());
        EXPECT(!installer.poll());                                              // reported once
        // It asks for exactly that release, from the download, leaving the running program alone.
        EXPECT(read_file(args) == "--download --keep-running --version v0.2.0\n");
    }
    {   // Autostart that is off stays off.
        ::setenv("FAKE_MODE", "ok", 1);
        UpdateInstaller installer = installer_for();
        EXPECT(installer.start("0.2.0", false) && finish(installer));
        EXPECT(read_file(args) == "--download --keep-running --version v0.2.0 --no-autostart\n");
    }

    struct Case {
        const char* mode;
        const char* expected;   // a part of the message
    };
    for (const Case& failing : {Case{"offline", "Could not download the update. Is the Frame online?"},
                                Case{"checksum", "checksum does not match"},
                                Case{"other", "tar is needed"},
                                Case{"two", "the real reason"},
                                Case{"silent", "ended with code 3"}}) {
        ::setenv("FAKE_MODE", failing.mode, 1);
        UpdateInstaller installer = installer_for();
        EXPECT(installer.start("0.2.0", true) && finish(installer));
        EXPECT(installer.status().state == InstallState::kFailed);
        EXPECT(contains(installer.status().message, failing.expected));
        if (std::string(failing.mode) == "two") EXPECT(!contains(installer.status().message, "first thing"));
        if (std::string(failing.mode) == "checksum") EXPECT(contains(installer.status().message, "Nothing was changed"));
    }
    {   // Raw messages are kept short enough for the panel.
        ::setenv("FAKE_MODE", "long", 1);
        UpdateInstaller installer = installer_for();
        EXPECT(installer.start("0.2.0", true) && finish(installer));
        EXPECT(installer.status().state == InstallState::kFailed);
        EXPECT(installer.status().message.size() <= 163U && contains(installer.status().message, "\xE2\x80\xA6"));
    }
    {   // A failure can be retried, and the retry starts from a clean slate.
        ::setenv("FAKE_MODE", "offline", 1);
        UpdateInstaller installer = installer_for();
        EXPECT(installer.start("0.2.0", true) && finish(installer));
        EXPECT(installer.status().state == InstallState::kFailed);
        ::setenv("FAKE_MODE", "ok", 1);
        EXPECT(installer.start("0.2.0", true));
        EXPECT(installer.status().state == InstallState::kInstalling && installer.status().message.empty());
        EXPECT(finish(installer) && installer.status().state == InstallState::kDone);
    }
    {   // One that takes too long is stopped.
        ::setenv("FAKE_MODE", "slow", 1);
        UpdateInstaller installer = installer_for(std::chrono::milliseconds(300));
        const auto begun = std::chrono::steady_clock::now();
        EXPECT(installer.start("0.2.0", true) && finish(installer));
        EXPECT(std::chrono::steady_clock::now() - begun < std::chrono::seconds(4));
        EXPECT(installer.status().state == InstallState::kFailed && contains(installer.status().message, "too long"));
    }

    // ---- Things that stop it from starting ----
    {
        fs::remove(args);
        UpdateInstaller::Options options;                                       // a copy that has no installer beside it
        UpdateInstaller installer(options);
        EXPECT(!installer.available());
        EXPECT(installer.start("0.2.0", true));
        EXPECT(installer.status().state == InstallState::kFailed && contains(installer.status().message, "install command"));
        EXPECT(!installer.poll());
    }
    {
        ::setenv("FAKE_MODE", "ok", 1);
        UpdateInstaller installer = installer_for();
        for (const char* bad : {"", "v0.2.0", "0.2.0; reboot", "$(id)", "../../x"}) {
            fs::remove(args);
            EXPECT(installer.start(bad, true));
            EXPECT(installer.status().state == InstallState::kFailed && contains(installer.status().message, "name"));
            EXPECT(!fs::exists(args));                                          // nothing was run
        }
    }
    {
        UpdateInstaller::Options options;
        options.script = script.string();
        options.bash = (work / "no-such-shell").string();
        UpdateInstaller installer(options);
        EXPECT(installer.start("0.2.0", true));
        // glibc says at once that there is no such program; bionic only shows it as the child's exit.
        if (installer.status().state == InstallState::kInstalling) EXPECT(finish(installer));
        EXPECT(installer.status().state == InstallState::kFailed);
        EXPECT(contains(installer.status().message, "Could not run the installer") ||
               contains(installer.status().message, "ended with code"));
        EXPECT(!installer.poll());
    }

    std::error_code ignored;
    fs::remove_all(work, ignored);

    if (failures != 0) {
        std::cerr << failures << " expectation(s) failed\n";
        return 1;
    }
    return 0;
}
