#include "system/update_install.h"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <sstream>
#include <utility>
#include <vector>

namespace frame_notify::system {
namespace {

constexpr std::size_t kMaximumOutput = 16384;   // the installer's own words; its error is the last line
constexpr std::size_t kMaximumMessage = 160;

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// What the installer said it failed on ("error: ..."), the last such line; empty when it said none.
std::string last_error_line(const std::string& output) {
    static const std::string marker = "error: ";
    std::istringstream lines(output);
    std::string line;
    std::string found;
    while (std::getline(lines, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.compare(0, marker.size(), marker) == 0) found = line.substr(marker.size());
    }
    return found;
}

std::string failure_message(int exit_code, const std::string& output) {
    const std::string error = last_error_line(output);
    if (error.empty()) return "The update failed (the installer ended with code " + std::to_string(exit_code) + ").";
    if (contains(error, "could not download")) return "Could not download the update. Is the Frame online?";
    if (contains(error, "checksum")) {
        return "The download is damaged (its checksum does not match). Nothing was changed.";
    }
    if (error.size() > kMaximumMessage) return error.substr(0, kMaximumMessage) + "\xE2\x80\xA6";
    return error;
}

}  // namespace

UpdateInstaller::UpdateInstaller(Options options) : options_(std::move(options)) {}

std::string UpdateInstaller::default_app_dir() {
    if (const char* configured = std::getenv("FRAME_NOTIFY_APP_DIR"); configured != nullptr && *configured != '\0') {
        return configured;
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') return {};
    return std::string(home) + "/.local/share/frame-notify";
}

std::string UpdateInstaller::find_script(const std::string& executable, const std::string& app_dir) {
    namespace fs = std::filesystem;
    if (executable.empty() || app_dir.empty()) return {};
    std::error_code error;
    const fs::path folder = fs::path(executable).parent_path();
    if (folder.empty() || !fs::equivalent(folder, app_dir, error) || error) return {};
    const fs::path script = folder / "install.sh";
    if (!fs::is_regular_file(script, error) || error) return {};
    return script.string();
}

bool UpdateInstaller::valid_version(const std::string& version) {
    if (version.empty() || version.size() > 32 || !std::isdigit(static_cast<unsigned char>(version.front()))) return false;
    for (const char character : version) {
        const auto letter = static_cast<unsigned char>(character);
        if (!std::isalnum(letter) && character != '.' && character != '-' && character != '_') return false;
    }
    return true;
}

void UpdateInstaller::fail(std::string message) {
    status_.state = InstallState::kFailed;
    status_.message = std::move(message);
}

bool UpdateInstaller::start(const std::string& version, bool autostart) {
    if (status_.state == InstallState::kInstalling) return false;
    status_ = InstallStatus{};
    if (!available()) {
        fail("This copy was not put in place by the installer, so it cannot update itself. "
             "Run the install command from the README again.");
        return true;
    }
    if (!valid_version(version)) {
        fail("The newest release has a name that cannot be installed from here.");
        return true;
    }
    // The installer downloads that release and checks it before it touches anything. It is told to
    // leave the running program alone: stopping the service would stop the installer with it.
    std::vector<std::string> command = {options_.bash, options_.script, "--download", "--keep-running",
                                        "--version", "v" + version};
    if (!autostart) command.emplace_back("--no-autostart");
    status_.state = InstallState::kInstalling;
    if (!process_.start(command, {}, options_.limit, kMaximumOutput)) {
        fail("Could not run the installer: " + process_.result().output);
    }
    return true;
}

bool UpdateInstaller::poll() {
    if (status_.state != InstallState::kInstalling) return false;
    if (!process_.poll()) return false;

    const ProcessResult& result = process_.result();
    if (result.timed_out) {
        fail("The update took too long and was stopped. Check that the Frame is online and try again.");
    } else if (result.exit_code == 0) {
        status_.state = InstallState::kDone;
    } else {
        fail(failure_message(result.exit_code, result.output));
    }
    return true;
}

}  // namespace frame_notify::system
