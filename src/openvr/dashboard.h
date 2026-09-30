#pragma once

#include "ui/history_view.h"
#include "ui/phone_info.h"
#include "ui/phone_view.h"
#include "ui/scroll_controller.h"
#include "ui/scroll_texture.h"
#include "ui/settings_info.h"

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace vr {
class IVROverlay;
}

namespace frame_notify::openvr {

enum class DashboardActionType {
    MarkRead,
    MarkAllRead,
    Dismiss,
    ClearAll,
    Exit,
    // Phone pairing, answered by the Bluetooth helper (see bluetooth::PhoneLink).
    PairStart,
    PairCancel,
    PairConfirm,
    PairReject,
    PairAnyway,
    RemoveConflict,    // `argument` is the Bluetooth address of the phone to remove
    PairDismiss,
    ForgetPhone,
    PowerOnBluetooth,
    RetryBluetooth,
    ToggleAutostart,   // settings: start with the Frame on or off
    CheckForUpdates,   // settings: ask whether a newer version exists
    InstallUpdate,     // settings: download and install the newer version, then restart
};

struct DashboardAction {
    DashboardActionType type;
    std::string notification_id;
    std::string argument;
};

class Dashboard {
public:
    Dashboard() = default;
    ~Dashboard();

    Dashboard(const Dashboard&) = delete;
    Dashboard& operator=(const Dashboard&) = delete;

    bool create(vr::IVROverlay* overlay_api);
    [[nodiscard]] std::vector<DashboardAction> poll_events();
    bool set_history(std::vector<ui::HistoryNotification> notifications,
                     bool reset_scroll = true);
    // Tells the panel what the Bluetooth helper reports. Pairing steps that need the user (a code
    // to compare, a result) bring the phone screen forward on their own.
    bool set_phone(ui::PhoneInfo phone);
    // What the settings screen shows.
    bool set_settings(ui::SettingsInfo settings);
    // Re-lays out the current screen against the clock, for example so "3 min ago" is right
    // before the dashboard appears. Nothing is uploaded when no pixel would change.
    bool refresh();
    [[nodiscard]] std::uint64_t main_handle() const noexcept { return main_handle_; }
    // How far the panel is scrolled, in pixels from the top of the content.
    [[nodiscard]] int scroll_offset() const noexcept { return uploaded_scroll_offset_; }
    // True once several calls to SteamVR in a row have failed: it has most likely gone away.
    [[nodiscard]] bool broken() const noexcept { return overlay_failures_ >= kFailuresBeforeBroken; }
    // Whether the dashboard panel is on screen right now.
    [[nodiscard]] bool visible() const noexcept { return dashboard_visible_; }
    // Whether the phone screen, rather than the notifications, is what the panel shows.
    [[nodiscard]] bool showing_phone_screen() const noexcept { return screen_ == Screen::kPhone; }
    [[nodiscard]] bool showing_settings() const noexcept { return screen_ == Screen::kSettings; }

private:
    enum class Screen {
        kNotifications,
        kPhone,
        kSettings,
    };

    [[nodiscard]] ui::HistoryContext make_context();
    [[nodiscard]] ui::PhoneInfo phone_with_elapsed_time() const;
    // A pointer event's position as a position on the panel (see ui::ScrollTexture::panel_point).
    [[nodiscard]] ui::PanelPoint pointer_on_panel(float mouse_x, float mouse_y) const;
    void handle_click(float x, float y, std::vector<DashboardAction>& actions);
    void handle_phone_click(float x, float y, std::vector<DashboardAction>& actions);
    void handle_settings_click(float x, float y, std::vector<DashboardAction>& actions);
    // The card screen (phone or settings) that is showing; only valid off the notifications screen.
    [[nodiscard]] const ui::PhoneView& card_view() const noexcept {
        return screen_ == Screen::kSettings ? settings_view_ : phone_view_;
    }
    void show_screen(Screen screen);
    void update_thumbnail(int unread);
    bool present(bool reset_scroll);
    bool upload_content();
    bool upload_window(int scroll_offset);
    bool update_scroll_view(int scroll_offset);
    [[nodiscard]] std::uint64_t current_signature() const noexcept;
    void destroy() noexcept;

    vr::IVROverlay* overlay_api_ = nullptr;
    std::uint64_t main_handle_ = 0;
    std::uint64_t thumbnail_handle_ = 0;
    ui::ScrollController scroll_;
    std::chrono::steady_clock::time_point last_tick_ = std::chrono::steady_clock::now();
    int uploaded_scroll_offset_ = 0;
    int notifications_scroll_ = 0;  // where the list was when the phone screen took over
    float mouse_x_ = 0.0F;
    float mouse_y_ = 0.0F;
    float press_x_ = 0.0F;
    float press_y_ = 0.0F;
    bool dashboard_visible_ = false;
    bool refresh_pending_ = false;
    int thumbnail_unread_ = 0;  // the unread count currently drawn on the dashboard tile
    // The overlay texture never changes size and has the viewport's shape (SteamVR sizes the panel
    // and its clickable area from the texture); it holds a window of `content_`.
    ui::ScrollTexture texture_;
    std::vector<std::uint8_t> content_;  // the current screen at full height
    int content_rows_ = 0;
    static constexpr int kFailuresBeforeBroken = 5;
    int overlay_failures_ = 0;       // consecutive failed calls that change what the overlay shows
    bool texture_replaced_ = false;  // the window moved since the last poll of the overlay's events
    std::uint64_t uploaded_signature_ = 0;
    Screen uploaded_screen_ = Screen::kNotifications;
    bool has_uploaded_ = false;
    std::int64_t rendered_minute_ = 0;
    std::int64_t rendered_second_ = 0;
    std::string expanded_id_;
    ui::HistoryView view_;

    Screen screen_ = Screen::kNotifications;
    ui::PhoneInfo phone_;
    std::chrono::steady_clock::time_point phone_changed_at_ = std::chrono::steady_clock::now();
    bool confirm_forget_ = false;  // the phone screen asks "forget this phone?"
    ui::PhoneView phone_view_;
    ui::SettingsInfo settings_;
    ui::PhoneView settings_view_;
};

}  // namespace frame_notify::openvr
