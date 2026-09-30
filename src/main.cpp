#include "bluetooth/phone_link.h"
#include "history/store.h"
#include "ipc/socket_server.h"
#include "openvr/dashboard.h"
#include "openvr/notifications.h"
#include "openvr/runtime.h"
#include "openvr/vr_session.h"
#include "system/autostart.h"
#include "system/single_instance.h"
#include "system/update_check.h"
#include "ui/app_style.h"
#include "ui/phone_info.h"
#include "ui/settings_info.h"
#include "ui/time_format.h"

#include <openvr.h>

#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifndef FRAME_NOTIFY_VERSION
#define FRAME_NOTIFY_VERSION "development"
#endif

namespace {

using frame_notify::openvr::DashboardActionType;

std::atomic_bool keep_running{true};

void handle_signal(int) {
    keep_running.store(false);
}

constexpr char kAfterSessionVariable[] = "FRAME_NOTIFY_AFTER_SESSION";
constexpr char kAttachFailuresVariable[] = "FRAME_NOTIFY_ATTACH_FAILURES";

void print_usage(std::string_view executable) {
    std::cout << "Usage: " << executable << " [options]\n"
              << "\n"
              << "Frame Notify runs in the background, keeps the iPhone connection and the notification\n"
              << "history going, and shows them in SteamVR whenever SteamVR is running.\n"
              << "\n"
              << "  --enable-autostart     Start Frame Notify by itself when the Frame starts.\n"
              << "  --disable-autostart    Stop doing that.\n"
              << "  --autostart-status     Say whether it does.\n"
              << "  --no-bluetooth         Do not start the iPhone Bluetooth helper (also set by\n"
              << "                         FRAME_NOTIFY_NO_BLUETOOTH=1).\n"
              << "  --diagnostics-only     Connect to SteamVR, print diagnostics, and exit.\n"
              << "  --native-notification  Ask SteamVR to show a real test notification.\n"
              << "  --version              Print the version.\n";
}

bool environment_flag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
}

int environment_number(const char* name, int fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    int parsed = 0;
    const std::string_view text(value);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? parsed : fallback;
}

frame_notify::ui::PhoneInfo make_phone_info(const frame_notify::bluetooth::PhoneStatus& status) {
    frame_notify::ui::PhoneInfo info;
    info.state = status.state;
    info.fields = status.fields;
    return info;
}

frame_notify::ui::PhoneInfo make_unavailable_phone_info(std::string message) {
    frame_notify::ui::PhoneInfo info;
    info.state = "helper_unavailable";
    info.fields["message"] = std::move(message);
    return info;
}

// What a toast should say while the user is not looking at the dashboard, or empty when a state
// needs no announcement. Pairing waits for an answer, so these are worth an interruption.
std::string pairing_toast(const frame_notify::ui::PhoneInfo& phone) {
    const std::string title = "iPhone pairing\n";
    if (phone.state == "pair_confirm") {
        const std::string code = phone.field("code");
        return title + (code.empty() ? std::string("Open Phone Notifications to allow the pairing")
                                     : "Code " + code + ": open Phone Notifications to confirm");
    }
    if (phone.state == "pair_done") return title + "Your iPhone is paired";
    if (phone.state == "pair_failed") return title + "Pairing did not finish";
    return {};
}

void log_runtime_event(const vr::VREvent_t& event) {
    switch (event.eventType) {
    case vr::VREvent_DashboardActivated:
        std::cout << "[Event] DashboardActivated\n";
        break;
    case vr::VREvent_DashboardDeactivated:
        std::cout << "[Event] DashboardDeactivated\n";
        break;
    case vr::VREvent_Notification_Shown:
        std::cout << "[Event] Notification_Shown id=" << event.data.notification.notificationId
                  << " userValue=" << event.data.notification.ulUserValue << '\n';
        break;
    case vr::VREvent_Notification_Hidden:
        std::cout << "[Event] Notification_Hidden id=" << event.data.notification.notificationId
                  << " userValue=" << event.data.notification.ulUserValue << '\n';
        break;
    case vr::VREvent_Notification_BeginInteraction:
        std::cout << "[Event] Notification_BeginInteraction id="
                  << event.data.notification.notificationId << " userValue="
                  << event.data.notification.ulUserValue << '\n';
        break;
    case vr::VREvent_Notification_Destroyed:
        std::cout << "[Event] Notification_Destroyed id="
                  << event.data.notification.notificationId << " userValue="
                  << event.data.notification.ulUserValue << '\n';
        break;
    default:
        break;
    }
}

std::uint64_t notification_user_value(std::string_view id) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const unsigned char byte : id) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

std::string single_line(std::string text) {
    for (char& character : text) {
        if (character == '\n' || character == '\r') character = ' ';
    }
    return text;
}

std::vector<frame_notify::ui::HistoryNotification> make_dashboard_history(
    const frame_notify::history::Store& store) {
    std::vector<frame_notify::ui::HistoryNotification> history;
    history.reserve(store.notifications().size());
    for (const auto& notification : store.notifications()) {
        if (notification.dismissed) continue;
        history.push_back({notification.id, notification.app, notification.title,
                           notification.message, notification.timestamp, notification.read,
                           notification.received_at, notification.app_id});
    }
    return history;
}

// The ids of the notifications the panel shows, that is, the ones not yet cleared.
std::vector<std::string> shown_ids(const frame_notify::history::Store& store) {
    std::vector<std::string> ids;
    for (const auto& notification : store.notifications()) {
        if (!notification.dismissed) ids.push_back(notification.id);
    }
    return ids;
}

int retention_setting(const char* name, int fallback, int minimum, int maximum) {
    const char* value = std::getenv(name);
    if (value == nullptr || *value == '\0') return fallback;
    int parsed = 0;
    const std::string_view text(value);
    const auto result = std::from_chars(text.data(), text.data() + text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != text.data() + text.size() || parsed < minimum ||
        parsed > maximum) {
        std::cerr << "[History] Ignoring invalid " << name << '=' << value << "; using "
                  << fallback << '\n';
        return fallback;
    }
    return parsed;
}

struct Options {
    bool diagnostics_only = false;
    bool native_notification = false;
    bool use_bluetooth = true;
    bool clear_on_phone = true;   // clearing a notification here clears it on the iPhone too
};

// How a run of the program ends: for good, or by starting over.
struct Outcome {
    bool restart = false;
    int exit_code = 0;
    int attach_failures = 0;    // consecutive, carried over a restart so the pauses can grow
    bool session_ended = false; // a SteamVR session just ended: the next one must be a new SteamVR
};

std::string current_executable() {
    std::error_code error;
    const auto path = std::filesystem::read_symlink("/proc/self/exe", error);
    return error ? std::string() : path.string();
}

frame_notify::ui::SettingsInfo read_settings(const frame_notify::system::Autostart& autostart) {
    frame_notify::ui::SettingsInfo settings;
    settings.version = FRAME_NOTIFY_VERSION;
    const auto status = autostart.status();
    settings.autostart_enabled = status.enabled;
    if (status.enabled) {
        settings.autostart_method =
            status.method == frame_notify::system::AutostartMethod::kDesktopEntry ? "desktop" : "systemd";
    }
    return settings;
}

void apply_update_status(frame_notify::ui::SettingsInfo& settings, const frame_notify::system::UpdateStatus& status) {
    using frame_notify::system::UpdateState;
    settings.update_latest = status.latest;
    settings.update_message = status.message;
    switch (status.state) {
    case UpdateState::kIdle: settings.update_state.clear(); break;
    case UpdateState::kChecking: settings.update_state = "checking"; break;
    case UpdateState::kCurrent: settings.update_state = "current"; break;
    case UpdateState::kAvailable: settings.update_state = "available"; break;
    case UpdateState::kUnknown: settings.update_state = "unknown"; break;
    case UpdateState::kFailed: settings.update_state = "failed"; break;
    }
}

// Where the newest release is announced: the repository's "releases/latest" address.
std::string releases_url() {
    const char* repository = std::getenv("FRAME_NOTIFY_REPO");
    return std::string("https://github.com/") +
           (repository != nullptr && *repository != '\0' ? repository : "Blackskydk/steam-frame-notify") +
           "/releases/latest";
}

int run_autostart_command(std::string_view command) {
    frame_notify::system::Autostart autostart(frame_notify::system::Autostart::default_options());
    std::string error;
    if (command == "--autostart-status") {
        const auto status = autostart.status();
        using frame_notify::system::AutostartMethod;
        std::cout << (status.enabled ? "Autostart is on" : "Autostart is off");
        if (status.enabled) {
            std::cout << (status.method == AutostartMethod::kDesktopEntry ? " (desktop autostart entry)"
                                                                          : " (systemd user service)");
        }
        std::cout << '\n';
        if (!status.detail.empty()) std::cout << status.detail << '\n';
        return 0;
    }
    if (command == "--enable-autostart") {
        if (!autostart.enable(error)) {
            std::cerr << "Could not turn autostart on: " << error << '\n';
            return 1;
        }
        std::cout << "Autostart is on: Frame Notify starts by itself when the Frame starts.\n";
        return 0;
    }
    if (!autostart.disable(error)) {
        std::cerr << "Could not turn autostart off: " << error << '\n';
        return 1;
    }
    std::cout << "Autostart is off. A running Frame Notify keeps running until you stop it.\n";
    return 0;
}

// `--probe-steamvr`: is SteamVR running? Answered by the exit code (0 yes, 1 no, 2 cannot tell);
// the background program asks this of a short-lived copy of itself (see VrSession).
int probe_steamvr() {
    std::string detail;
    switch (frame_notify::openvr::Runtime::probe(detail)) {
    case frame_notify::openvr::SteamVrState::kRunning:
        return 0;
    case frame_notify::openvr::SteamVrState::kNotRunning:
        return 1;
    case frame_notify::openvr::SteamVrState::kUnavailable:
        break;
    }
    std::cout << detail;
    return 2;
}

Outcome run(const Options& options, const std::string& executable, bool after_session, int attach_failures) {
    using namespace frame_notify;

    ipc::SocketServer socket_server;
    if (!socket_server.start()) return {false, 1, 0, false};
    const int maximum_notifications =
        retention_setting("FRAME_NOTIFY_MAX_NOTIFICATIONS", 20, 1, 50);
    const int maximum_age_days = retention_setting("FRAME_NOTIFY_MAX_AGE_DAYS", 30, 1, 365);
    history::Store history(static_cast<std::size_t>(maximum_notifications), maximum_age_days);
    if (!history.initialize()) return {false, 1, 0, false};
    std::cout << "[History] Retention: " << maximum_notifications << " notification(s), "
              << maximum_age_days << " day(s)\n";
    // Times on the cards come from this clock and zone; if they look wrong, compare this line
    // with the real local time (`date` on the Frame).
    std::cout << "[UI] Local time: "
              << ui::describe_local_time(std::chrono::duration_cast<std::chrono::seconds>(
                                             std::chrono::system_clock::now().time_since_epoch())
                                             .count())
              << '\n';

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    // The Bluetooth helper pairs and listens to the iPhone. It is optional: without it, or when
    // it cannot run, the panel says so and notifications still arrive through the local socket.
    bluetooth::PhoneLink phone_link;
    std::string phone_state;  // the helper's last state, to announce changes once
    ui::PhoneInfo phone_info;
    if (!options.use_bluetooth) {
        std::cout << "[Bluetooth] Disabled; not starting the iPhone helper\n";
        phone_info = make_unavailable_phone_info("Bluetooth is turned off for this run.");
    } else {
        auto helper_command = bluetooth::default_helper_command();
        if (helper_command.empty()) {
            std::cerr << "[Bluetooth] Could not find scripts/ancs_bridge.py; set "
                         "FRAME_NOTIFY_BRIDGE to its path\n";
        } else {
            std::cout << "[Bluetooth] Starting the iPhone helper:";
            for (const auto& part : helper_command) std::cout << ' ' << part;
            std::cout << '\n';
        }
        phone_link.start(std::move(helper_command));
        phone_info = make_phone_info(phone_link.status());
    }
    const auto send_to_phone = [&](std::string_view command,
                                   const std::vector<std::pair<std::string, std::string>>& fields = {}) {
        if (!options.use_bluetooth || !phone_link.send(command, fields)) {
            std::cerr << "[Bluetooth] The helper is not running; dropped command '" << command << "'\n";
        }
    };

    // What is cleared here is cleared on the iPhone too, where the iPhone allows it. The helper
    // only ever sees notifications it sent; with no helper running the iPhone simply keeps them
    // (and the Frame does not show them again: the helper remembers what it has sent).
    const auto clear_on_phone = [&](const std::vector<std::string>& ids) {
        if (!options.clear_on_phone || !options.use_bluetooth) return;
        const auto field = bluetooth::clear_notifications_field(ids);
        if (!field.empty() && phone_link.send("clear_notifications", {{"ids", field}})) {
            std::cout << "[Bluetooth] Asked the iPhone to clear what was cleared here\n";
        }
    };

    system::Autostart autostart(system::Autostart::default_options());
    ui::SettingsInfo settings = read_settings(autostart);
    system::UpdateChecker updates({FRAME_NOTIFY_VERSION, releases_url()});

    // SteamVR: the panel and the toasts exist only while SteamVR runs. A pause before the first
    // attempt grows with each failed connection, so a SteamVR that cannot be used is not hammered.
    openvr::VrSession::Options session_options;
    session_options.executable = executable;
    session_options.plan.wait_for_shutdown_first = after_session;
    if (attach_failures > 0) {
        session_options.plan.initial_delay = std::chrono::seconds(
            std::min(60, 5 << std::min(attach_failures - 1, 4)));
    }
    openvr::VrSession vr(session_options);
    bool test_toast_pending = options.native_notification;

    std::cout << "[Frame Notify] " << FRAME_NOTIFY_VERSION
              << " is running in the background; the dashboard entry appears when SteamVR is running\n";

    Outcome outcome;
    while (keep_running.load()) {
        if (phone_link.poll()) {
            const auto previous_state = phone_state;
            phone_state = phone_link.status().state;
            phone_info = make_phone_info(phone_link.status());
            if (vr.attached()) {
                vr.dashboard().set_phone(phone_info);
                if (phone_state != previous_state && !vr.dashboard().visible()) {
                    if (const auto text = pairing_toast(phone_info); !text.empty()) {
                        vr.notification().show(notification_user_value("phone-pairing"), text);
                    }
                }
            }
        }

        for (auto& incoming : socket_server.poll()) {
            incoming.received_at = std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
            incoming.read = false;
            incoming.dismissed = false;
            const bool show_toast = incoming.toast;
            const std::string id = incoming.id;
            if (!history.add(std::move(incoming))) {
                std::cout << "[IPC] Ignored duplicate notification id=" << id << '\n';
                continue;
            }

            const auto& current = history.notifications().front();
            std::cout << "[IPC] Notification id=" << current.id << " app=" << current.app
                      << " toast=" << (show_toast ? "yes" : "no") << " dashboard="
                      << (!vr.attached() ? "no-steamvr" : vr.dashboard().visible() ? "open" : "closed")
                      << '\n';
            if (!vr.attached()) continue;   // kept in the history for when SteamVR is up
            if (!vr.dashboard().set_history(make_dashboard_history(history))) {
                std::cerr << "[Dashboard] Failed to refresh notification history\n";
            }
            // Older senders may pass a bundle identifier as the app; show a readable name.
            const std::string app_name = ui::resolve_app_style(current.app, current.app_id).name;
            std::string toast = single_line(app_name) + "\n" + single_line(current.title);
            if (current.message != current.title) toast += ": " + single_line(current.message);
            if (show_toast && !vr.notification().show(notification_user_value(current.id), toast)) {
                std::cerr << "[Notification] Native toast failed for id=" << current.id << '\n';
            }
        }

        if (updates.poll()) {
            const auto& found = updates.status();
            if (found.state == system::UpdateState::kFailed) {
                std::cerr << "[Update] Check failed: " << found.message << '\n';
            } else {
                std::cout << "[Update] Newest release: " << found.latest << " (this is " << FRAME_NOTIFY_VERSION
                          << ")\n";
            }
            apply_update_status(settings, found);
            if (vr.attached()) vr.dashboard().set_settings(settings);
        }

        if (!vr.attached()) {
            const bool connected = vr.poll(std::chrono::steady_clock::now());
            if (vr.failed()) {
                std::cerr << "[VR] Could not use SteamVR; starting over shortly\n";
                outcome = {true, 0, attach_failures + 1, false};
                break;
            }
            if (connected) {
                vr.dashboard().set_history(make_dashboard_history(history));
                vr.dashboard().set_phone(phone_info);
                vr.dashboard().set_settings(settings);
                if (test_toast_pending) {
                    vr.notification().show_test();
                    test_toast_pending = false;
                }
                std::cout << "[Dashboard] Phone Notifications is ready\n"
                          << "[Dashboard] Open the SteamVR Dashboard and select the entry\n";
            }
        } else {
            auto& dashboard = vr.dashboard();
            bool history_changed = false;
            for (const auto& action : dashboard.poll_events()) {
                switch (action.type) {
                case DashboardActionType::MarkRead:
                    if (history.mark_read(action.notification_id)) {
                        std::cout << "[History] Marked read id=" << action.notification_id << '\n';
                        history_changed = true;
                    }
                    break;
                case DashboardActionType::MarkAllRead:
                    if (const auto count = history.mark_all_read(); count != 0U) {
                        std::cout << "[History] Marked " << count << " notification(s) read\n";
                        history_changed = true;
                    }
                    break;
                case DashboardActionType::Dismiss: {
                    const auto shown = shown_ids(history);
                    const bool was_shown = std::find(shown.begin(), shown.end(),
                                                     action.notification_id) != shown.end();
                    if (history.dismiss(action.notification_id)) {
                        std::cout << "[History] Cleared local notification id="
                                  << action.notification_id << '\n';
                        history_changed = true;
                        if (was_shown) clear_on_phone({action.notification_id});
                    }
                    break;
                }
                case DashboardActionType::ClearAll: {
                    const auto shown = shown_ids(history);
                    if (const auto count = history.dismiss_all(); count != 0U) {
                        std::cout << "[History] Cleared " << count << " local notification(s)\n";
                        history_changed = true;
                        clear_on_phone(shown);
                    }
                    break;
                }
                case DashboardActionType::Exit:
                    // The close button on the panel's control bar. The program itself keeps
                    // running; only the dashboard entry goes, until SteamVR starts again.
                    std::cout << "[Dashboard] Close requested; leaving SteamVR until it restarts\n";
                    outcome = {true, 0, 0, true};
                    break;
                case DashboardActionType::PairStart:
                    send_to_phone("pair");
                    break;
                case DashboardActionType::PairCancel:
                    send_to_phone("cancel");
                    break;
                case DashboardActionType::PairConfirm:
                    send_to_phone("confirm");
                    break;
                case DashboardActionType::PairReject:
                    send_to_phone("reject");
                    break;
                case DashboardActionType::PairAnyway:
                    send_to_phone("pair_anyway");
                    break;
                case DashboardActionType::RemoveConflict:
                    send_to_phone("remove_conflict", {{"address", action.argument}});
                    break;
                case DashboardActionType::PairDismiss:
                    send_to_phone("dismiss");
                    break;
                case DashboardActionType::ForgetPhone:
                    send_to_phone("forget");
                    break;
                case DashboardActionType::PowerOnBluetooth:
                    send_to_phone("power_on");
                    break;
                case DashboardActionType::RetryBluetooth:
                    if (options.use_bluetooth) phone_link.retry();
                    break;
                case DashboardActionType::ToggleAutostart: {
                    const bool was_enabled = settings.autostart_enabled;
                    std::string error;
                    const bool done = was_enabled ? autostart.disable(error) : autostart.enable(error);
                    settings = read_settings(autostart);
                    apply_update_status(settings, updates.status());
                    if (!done) {
                        settings.message = std::string("Could not turn autostart ") +
                                           (was_enabled ? "off: " : "on: ") + error;
                        std::cerr << "[Autostart] " << settings.message << '\n';
                    } else {
                        std::cout << "[Autostart] Turned " << (was_enabled ? "off" : "on") << '\n';
                    }
                    dashboard.set_settings(settings);
                    break;
                }
                case DashboardActionType::CheckForUpdates:
                    if (updates.start()) {
                        std::cout << "[Update] Asking github.com for the newest release\n";
                        if (updates.status().state == system::UpdateState::kFailed) {
                            std::cerr << "[Update] Check failed: " << updates.status().message << '\n';
                        }
                        apply_update_status(settings, updates.status());
                        dashboard.set_settings(settings);
                    }
                    break;
                }
            }
            if (outcome.restart) break;
            if (history_changed && !dashboard.set_history(make_dashboard_history(history), false)) {
                std::cerr << "[Dashboard] Failed to refresh notification state\n";
            }

            vr::VREvent_t event{};
            while (vr.runtime().system()->PollNextEvent(&event, sizeof(event))) {
                log_runtime_event(event);
                // Bring the times up to date while the dashboard is still opening, not after it shows.
                if (event.eventType == vr::VREvent_DashboardActivated) dashboard.refresh();
                if (event.eventType == vr::VREvent_Quit) {
                    std::cout << "[OpenVR] SteamVR requested shutdown\n";
                    vr.runtime().system()->AcknowledgeQuit_Exiting();
                    outcome = {true, 0, 0, true};
                    break;
                }
            }
            if (outcome.restart) break;
            if (dashboard.broken()) {
                std::cerr << "[VR] SteamVR stopped answering; starting over\n";
                outcome = {true, 0, 0, true};
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    // Overlays first, while OpenVR is still connected; then the helper.
    vr.detach();
    phone_link.stop();
    return outcome;
}

}  // namespace

int main(int argc, char* argv[]) {
    // Logs go to a terminal or to the journal; either way each line should appear when it is written.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    Options options;
    options.use_bluetooth = !environment_flag("FRAME_NOTIFY_NO_BLUETOOTH");
    options.clear_on_phone = !environment_flag("FRAME_NOTIFY_KEEP_ON_PHONE");
    std::vector<std::string> arguments;
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--probe-steamvr") {
            return probe_steamvr();
        }
        if (argument == "--version") {
            std::cout << "frame-notify " << FRAME_NOTIFY_VERSION << '\n';
            return 0;
        }
        if (argument == "--enable-autostart" || argument == "--disable-autostart" ||
            argument == "--autostart-status") {
            return run_autostart_command(argument);
        }
        if (argument == "--diagnostics-only") {
            options.diagnostics_only = true;
        } else if (argument == "--native-notification") {
            options.native_notification = true;
            continue;   // a test for one run only: not repeated when the program starts over
        } else if (argument == "--no-bluetooth") {
            options.use_bluetooth = false;
        } else if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argument << "\n";
            print_usage(argv[0]);
            return 2;
        }
        arguments.emplace_back(argument);
    }

    if (options.diagnostics_only) {
        frame_notify::openvr::Runtime runtime;
        if (!runtime.initialize()) return 1;
        std::cout << "[OpenVR] Diagnostics completed successfully\n";
        return 0;
    }

    frame_notify::system::SingleInstance instance;
    if (!instance.acquire(frame_notify::system::SingleInstance::default_directory())) {
        std::cerr << "[Frame Notify] Not starting: " << instance.error();
        if (instance.holder() > 0) std::cerr << " (process " << instance.holder() << ")";
        std::cerr << ".\nIf it was started by the background service, stop that first with "
                     "`systemctl --user stop frame-notify`.\n";
        return 1;
    }

    const std::string executable = current_executable();
    const bool after_session = environment_flag(kAfterSessionVariable);
    const int attach_failures = environment_number(kAttachFailuresVariable, 0);
    const Outcome outcome = run(options, executable, after_session, attach_failures);
    if (!outcome.restart) return outcome.exit_code;

    // A SteamVR session is over. Start over as a fresh process, which can connect to the next
    // SteamVR cleanly; everything the old process held has been released by now.
    setenv(kAfterSessionVariable, outcome.session_ended ? "1" : "0", 1);
    setenv(kAttachFailuresVariable, std::to_string(outcome.attach_failures).c_str(), 1);
    std::vector<std::string> restart_arguments = {executable};
    restart_arguments.insert(restart_arguments.end(), arguments.begin(), arguments.end());
    std::vector<char*> restart_argv;
    for (auto& item : restart_arguments) restart_argv.push_back(item.data());
    restart_argv.push_back(nullptr);
    std::cout << "[Frame Notify] Starting over\n";
    std::cout.flush();
    if (!executable.empty()) execv(executable.c_str(), restart_argv.data());
    std::cerr << "[Frame Notify] Could not start over; exiting\n";
    return 1;
}
