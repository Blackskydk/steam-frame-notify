#pragma once

#include <string>
#include <vector>

namespace frame_notify::system {

enum class AutostartMethod {
    kNone,          // not set up
    kSystemd,       // a systemd user service: starts when the user's session starts (at boot on the Frame)
    kDesktopEntry,  // an XDG autostart entry, for a system without a systemd user manager
};

struct AutostartStatus {
    AutostartMethod method = AutostartMethod::kNone;
    bool enabled = false;   // Frame Notify starts by itself the next time the Frame starts
    std::string detail;     // a short sentence for when something is off
};

// Lets Frame Notify start on its own when the Frame starts, by installing a systemd user service
// (or, where there is none, an XDG autostart entry). Everything lives in the user's own
// configuration directory; nothing outside the home directory is touched and no root is needed.
class Autostart {
public:
    struct Options {
        std::string executable;                  // the program to start
        std::string config_home;                 // $XDG_CONFIG_HOME, or ~/.config
        std::string systemctl = "systemctl";     // the command that talks to systemd
        std::string runtime_dir;                 // the user's runtime dir; /run/user/<uid> when empty
    };

    explicit Autostart(Options options);
    // This executable, in the current user's configuration directory.
    [[nodiscard]] static Options default_options();

    [[nodiscard]] AutostartStatus status() const;
    // Returns false and says why in `error` when it could not be set up.
    bool enable(std::string& error);
    // Removes whatever enable() set up. Does not stop a running Frame Notify, which would end the
    // very program that asked. Returns false and says why in `error` when something is left.
    bool disable(std::string& error);

    [[nodiscard]] std::string unit_path() const;
    [[nodiscard]] std::string desktop_entry_path() const;
    [[nodiscard]] static std::string unit_text(const std::string& executable);
    [[nodiscard]] static std::string desktop_entry_text(const std::string& executable);
    // `text` as one argument of a systemd command line.
    [[nodiscard]] static std::string systemd_quote(const std::string& text);

private:
    struct CommandResult {
        bool started = false;
        int exit_code = -1;
        std::string output;
    };
    [[nodiscard]] CommandResult systemctl(const std::vector<std::string>& arguments) const;
    [[nodiscard]] bool write_file(const std::string& path, const std::string& text,
                                  std::string& error) const;

    Options options_;
};

}  // namespace frame_notify::system
