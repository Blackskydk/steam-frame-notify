#include "bluetooth/phone_link.h"
#include "history/store.h"
#include "ipc/socket_server.h"
#include "openvr/dashboard.h"
#include "openvr/notifications.h"
#include "openvr/runtime.h"
#include "ui/app_style.h"
#include "ui/phone_info.h"
#include "ui/time_format.h"

#include <openvr.h>

#include <atomic>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

namespace {

std::atomic_bool keep_running{true};

void handle_signal(int) {
    keep_running.store(false);
}

void print_usage(std::string_view executable) {
    std::cout << "Usage: " << executable
              << " [--diagnostics-only] [--native-notification] [--no-bluetooth]\n"
              << "\n"
              << "  --diagnostics-only     Initialize OpenVR, print diagnostics, and exit.\n"
              << "  --native-notification  Ask SteamVR to show a real test notification.\n"
              << "  --no-bluetooth         Do not start the iPhone Bluetooth helper (also set by\n"
              << "                         FRAME_NOTIFY_NO_BLUETOOTH=1).\n";
}

bool environment_flag(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' && std::string_view(value) != "0";
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

}  // namespace

int main(int argc, char* argv[]) {
    bool diagnostics_only = false;
    bool native_notification = false;
    bool use_bluetooth = !environment_flag("FRAME_NOTIFY_NO_BLUETOOTH");
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument{argv[index]};
        if (argument == "--diagnostics-only") {
            diagnostics_only = true;
        } else if (argument == "--native-notification") {
            native_notification = true;
        } else if (argument == "--no-bluetooth") {
            use_bluetooth = false;
        } else if (argument == "--help" || argument == "-h") {
            print_usage(argv[0]);
            return 0;
        } else {
            std::cerr << "Unknown option: " << argument << "\n";
            print_usage(argv[0]);
            return 2;
        }
    }

    frame_notify::openvr::Runtime runtime;
    if (!runtime.initialize()) {
        return 1;
    }

    if (diagnostics_only) {
        std::cout << "[OpenVR] Diagnostics completed successfully\n";
        return 0;
    }

    frame_notify::openvr::Dashboard dashboard;
    if (!dashboard.create(runtime.overlay())) {
        return 1;
    }

    frame_notify::openvr::NativeNotification notification;
    if (!notification.bind(runtime.notifications(), dashboard.main_handle())) {
        return 1;
    }
    if (native_notification && !notification.show_test()) {
        return 1;
    }

    frame_notify::ipc::SocketServer socket_server;
    if (!socket_server.start()) {
        return 1;
    }
    const int maximum_notifications =
        retention_setting("FRAME_NOTIFY_MAX_NOTIFICATIONS", 20, 1, 50);
    const int maximum_age_days = retention_setting("FRAME_NOTIFY_MAX_AGE_DAYS", 30, 1, 365);
    frame_notify::history::Store history(static_cast<std::size_t>(maximum_notifications),
                                         maximum_age_days);
    if (!history.initialize() || !dashboard.set_history(make_dashboard_history(history))) {
        return 1;
    }
    std::cout << "[History] Retention: " << maximum_notifications << " notification(s), "
              << maximum_age_days << " day(s)\n";
    // Times on the cards come from this clock and zone; if they look wrong, compare this line
    // with the real local time (`date` on the Frame).
    std::cout << "[UI] Local time: "
              << frame_notify::ui::describe_local_time(
                     std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count())
              << '\n';

    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    // The Bluetooth helper pairs and listens to the iPhone. It is optional: without it, or when
    // it cannot run, the panel says so and notifications still arrive through the local socket.
    frame_notify::bluetooth::PhoneLink phone_link;
    std::string phone_state;  // the helper's last state, to announce changes once
    if (!use_bluetooth) {
        std::cout << "[Bluetooth] Disabled; not starting the iPhone helper\n";
        dashboard.set_phone(make_unavailable_phone_info("Bluetooth is turned off for this run."));
    } else {
        auto helper_command = frame_notify::bluetooth::default_helper_command();
        if (helper_command.empty()) {
            std::cerr << "[Bluetooth] Could not find scripts/ancs_bridge.py; set "
                         "FRAME_NOTIFY_BRIDGE to its path\n";
        } else {
            std::cout << "[Bluetooth] Starting the iPhone helper:";
            for (const auto& part : helper_command) std::cout << ' ' << part;
            std::cout << '\n';
        }
        phone_link.start(std::move(helper_command));
        dashboard.set_phone(make_phone_info(phone_link.status()));
    }

    const auto send_to_phone = [&](std::string_view command,
                                   const std::vector<std::pair<std::string, std::string>>& fields = {}) {
        if (!use_bluetooth || !phone_link.send(command, fields)) {
            std::cerr << "[Bluetooth] The helper is not running; dropped command '" << command << "'\n";
        }
    };

    std::cout << "[Dashboard] Phone Notifications is ready\n"
              << "[Dashboard] Open the SteamVR Dashboard and select the entry; press Ctrl+C to stop\n";

    while (keep_running.load()) {
        if (phone_link.poll()) {
            const auto previous_state = phone_state;
            phone_state = phone_link.status().state;
            const auto phone = make_phone_info(phone_link.status());
            dashboard.set_phone(phone);
            if (phone_state != previous_state && !dashboard.visible()) {
                if (const auto text = pairing_toast(phone); !text.empty()) {
                    notification.show(notification_user_value("phone-pairing"), text);
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
                      << " toast=" << (show_toast ? "yes" : "no")
                      << " dashboard=" << (dashboard.visible() ? "open" : "closed") << '\n';
            if (!dashboard.set_history(make_dashboard_history(history))) {
                std::cerr << "[Dashboard] Failed to refresh notification history\n";
            }
            // Older senders may pass a bundle identifier as the app; show a readable name.
            const std::string app_name =
                frame_notify::ui::resolve_app_style(current.app, current.app_id).name;
            std::string toast = single_line(app_name) + "\n" + single_line(current.title);
            if (current.message != current.title) toast += ": " + single_line(current.message);
            if (show_toast && !notification.show(notification_user_value(current.id), toast)) {
                std::cerr << "[Notification] Native toast failed for id=" << current.id << '\n';
            }
        }
        const auto dashboard_actions = dashboard.poll_events();
        bool history_changed = false;
        for (const auto& action : dashboard_actions) {
            switch (action.type) {
            case frame_notify::openvr::DashboardActionType::MarkRead:
                if (history.mark_read(action.notification_id)) {
                    std::cout << "[History] Marked read id=" << action.notification_id << '\n';
                    history_changed = true;
                }
                break;
            case frame_notify::openvr::DashboardActionType::MarkAllRead:
                if (const auto count = history.mark_all_read(); count != 0U) {
                    std::cout << "[History] Marked " << count << " notification(s) read\n";
                    history_changed = true;
                }
                break;
            case frame_notify::openvr::DashboardActionType::Dismiss:
                if (history.dismiss(action.notification_id)) {
                    std::cout << "[History] Cleared local notification id="
                              << action.notification_id << '\n';
                    history_changed = true;
                }
                break;
            case frame_notify::openvr::DashboardActionType::ClearAll:
                if (const auto count = history.dismiss_all(); count != 0U) {
                    std::cout << "[History] Cleared " << count << " local notification(s)\n";
                    history_changed = true;
                }
                break;
            case frame_notify::openvr::DashboardActionType::Exit:
                std::cout << "[Dashboard] Close requested; stopping Frame Notify\n";
                keep_running.store(false);
                break;
            case frame_notify::openvr::DashboardActionType::PairStart:
                send_to_phone("pair");
                break;
            case frame_notify::openvr::DashboardActionType::PairCancel:
                send_to_phone("cancel");
                break;
            case frame_notify::openvr::DashboardActionType::PairConfirm:
                send_to_phone("confirm");
                break;
            case frame_notify::openvr::DashboardActionType::PairReject:
                send_to_phone("reject");
                break;
            case frame_notify::openvr::DashboardActionType::PairAnyway:
                send_to_phone("pair_anyway");
                break;
            case frame_notify::openvr::DashboardActionType::RemoveConflict:
                send_to_phone("remove_conflict", {{"address", action.argument}});
                break;
            case frame_notify::openvr::DashboardActionType::PairDismiss:
                send_to_phone("dismiss");
                break;
            case frame_notify::openvr::DashboardActionType::ForgetPhone:
                send_to_phone("forget");
                break;
            case frame_notify::openvr::DashboardActionType::PowerOnBluetooth:
                send_to_phone("power_on");
                break;
            case frame_notify::openvr::DashboardActionType::RetryBluetooth:
                if (use_bluetooth) phone_link.retry();
                break;
            }
        }
        if (history_changed && !dashboard.set_history(make_dashboard_history(history), false)) {
            std::cerr << "[Dashboard] Failed to refresh notification state\n";
        }
        vr::VREvent_t event{};
        while (runtime.system()->PollNextEvent(&event, sizeof(event))) {
            log_runtime_event(event);
            // Bring the times up to date while the dashboard is still opening, not after it shows.
            if (event.eventType == vr::VREvent_DashboardActivated) dashboard.refresh();
            if (event.eventType == vr::VREvent_Quit) {
                std::cout << "[OpenVR] SteamVR requested shutdown\n";
                runtime.system()->AcknowledgeQuit_Exiting();
                keep_running.store(false);
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }

    phone_link.stop();
    return 0;
}
