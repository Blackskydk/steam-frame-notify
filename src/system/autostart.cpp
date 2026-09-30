#include "system/autostart.h"

#include "system/process.h"

#include <filesystem>
#include <fstream>
#include <utility>

#include <unistd.h>

namespace frame_notify::system {
namespace {

namespace fs = std::filesystem;

constexpr char kUnitName[] = "frame-notify.service";

}  // namespace

Autostart::Autostart(Options options) : options_(std::move(options)) {}

Autostart::Options Autostart::default_options() {
    Options options;
    std::error_code error;
    const fs::path executable = fs::read_symlink("/proc/self/exe", error);
    if (!error) options.executable = executable.string();
    if (const char* configured = std::getenv("XDG_CONFIG_HOME"); configured != nullptr && *configured != '\0') {
        options.config_home = configured;
    } else if (const char* home = std::getenv("HOME"); home != nullptr && *home != '\0') {
        options.config_home = std::string(home) + "/.config";
    }
    return options;
}

std::string Autostart::unit_path() const {
    return options_.config_home + "/systemd/user/" + kUnitName;
}

std::string Autostart::desktop_entry_path() const {
    return options_.config_home + "/autostart/frame-notify.desktop";
}

std::string Autostart::systemd_quote(const std::string& text) {
    std::string quoted = "\"";
    for (const char character : text) {
        if (character == '"' || character == '\\') quoted += '\\';
        quoted += character;
        if (character == '%' || character == '$') quoted += character;   // doubled: no specifier/variable
    }
    return quoted + "\"";
}

std::string Autostart::unit_text(const std::string& executable) {
    return "[Unit]\n"
           "Description=Frame Notify: iPhone notifications in SteamVR\n"
           "\n"
           "[Service]\n"
           "Type=simple\n"
           "ExecStart=" + systemd_quote(executable) + "\n"
           "Restart=on-failure\n"
           "RestartSec=5\n"
           "\n"
           "[Install]\n"
           "WantedBy=default.target\n";
}

std::string Autostart::desktop_entry_text(const std::string& executable) {
    // The Exec value is quoted the way the desktop entry specification asks: double quotes, with
    // backslashes, quotes, dollar signs and backticks escaped.
    std::string quoted = "\"";
    for (const char character : executable) {
        if (character == '"' || character == '\\' || character == '$' || character == '`') quoted += '\\';
        quoted += character;
    }
    quoted += "\"";
    return "[Desktop Entry]\n"
           "Type=Application\n"
           "Name=Frame Notify\n"
           "Comment=iPhone notifications in SteamVR\n"
           "Exec=" + quoted + "\n"
           "Terminal=false\n"
           "X-GNOME-Autostart-enabled=true\n";
}

Autostart::CommandResult Autostart::systemctl(const std::vector<std::string>& arguments) const {
    // `systemctl --user` finds the user's systemd through these; a shell started over SSH does not
    // always have them.
    const std::string runtime_dir = !options_.runtime_dir.empty() ? options_.runtime_dir
                                                                  : "/run/user/" + std::to_string(::getuid());
    std::vector<std::string> command = {options_.systemctl, "--user"};
    command.insert(command.end(), arguments.begin(), arguments.end());
    const ProcessResult process = run_process(
        command, {{"XDG_RUNTIME_DIR", runtime_dir}, {"DBUS_SESSION_BUS_ADDRESS", "unix:path=" + runtime_dir + "/bus"}});
    CommandResult result;
    result.started = process.started;
    result.exit_code = process.exit_code;
    result.output = process.output;
    return result;
}

bool Autostart::write_file(const std::string& path, const std::string& text, std::string& error) const {
    std::error_code code;
    fs::create_directories(fs::path(path).parent_path(), code);
    if (code) {
        error = "cannot create " + fs::path(path).parent_path().string() + ": " + code.message();
        return false;
    }
    const std::string temporary = path + ".tmp";
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream << text;
        stream.flush();
        if (!stream) {
            error = "cannot write " + temporary;
            return false;
        }
    }
    fs::rename(temporary, path, code);
    if (code) {
        error = "cannot replace " + path + ": " + code.message();
        fs::remove(temporary, code);
        return false;
    }
    return true;
}

AutostartStatus Autostart::status() const {
    AutostartStatus result;
    std::error_code code;
    if (fs::exists(unit_path(), code)) {
        result.method = AutostartMethod::kSystemd;
        const CommandResult enabled = systemctl({"is-enabled", kUnitName});
        result.enabled = enabled.started && enabled.exit_code == 0;
        if (!result.enabled) result.detail = "The service file exists but is not enabled.";
        return result;
    }
    if (fs::exists(desktop_entry_path(), code)) {
        result.method = AutostartMethod::kDesktopEntry;
        result.enabled = true;
    }
    return result;
}

bool Autostart::enable(std::string& error) {
    error.clear();
    if (options_.executable.empty() || options_.config_home.empty()) {
        error = "cannot tell where Frame Notify or the configuration directory is";
        return false;
    }

    // Preferred: a systemd user service. If any step fails the file is withdrawn again, so a
    // half-done service never lingers, and the desktop entry is tried instead.
    std::string systemd_problem;
    if (write_file(unit_path(), unit_text(options_.executable), systemd_problem)) {
        const CommandResult reload = systemctl({"daemon-reload"});
        const CommandResult enable = reload.started && reload.exit_code == 0
                                         ? systemctl({"enable", kUnitName})
                                         : reload;
        if (enable.started && enable.exit_code == 0) {
            std::error_code code;
            fs::remove(desktop_entry_path(), code);   // never start twice
            return true;
        }
        systemd_problem = enable.output.empty() ? "systemctl failed" : enable.output;
        std::error_code code;
        fs::remove(unit_path(), code);
    }

    std::string entry_problem;
    if (write_file(desktop_entry_path(), desktop_entry_text(options_.executable), entry_problem)) return true;
    error = "systemd: " + systemd_problem + "; autostart entry: " + entry_problem;
    return false;
}

bool Autostart::disable(std::string& error) {
    error.clear();
    std::error_code code;
    if (fs::exists(unit_path(), code)) {
        const CommandResult disabled = systemctl({"disable", kUnitName});
        fs::remove(unit_path(), code);
        if (code) {
            error = "cannot remove " + unit_path() + ": " + code.message();
            return false;
        }
        static_cast<void>(systemctl({"daemon-reload"}));
        if (disabled.started && disabled.exit_code != 0 && !disabled.output.empty()) {
            // The file is gone, so it cannot start; say what systemd objected to all the same.
            error = disabled.output;
        }
    }
    code.clear();
    fs::remove(desktop_entry_path(), code);
    if (code) {
        error = "cannot remove " + desktop_entry_path() + ": " + code.message();
        return false;
    }
    const bool clean = !fs::exists(unit_path(), code) && !fs::exists(desktop_entry_path(), code);
    if (clean) error.clear();   // whatever systemd grumbled about, nothing is left to start
    return clean;
}

}  // namespace frame_notify::system
