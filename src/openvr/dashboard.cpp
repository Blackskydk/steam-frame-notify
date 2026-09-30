#include "openvr/dashboard.h"

#include "ui/history_view.h"
#include "ui/icon.h"
#include "ui/time_format.h"

#include <openvr.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <utility>

namespace frame_notify::openvr {
namespace {

constexpr char kOverlayKey[] = "com.frame-notify.dashboard";
constexpr char kFriendlyName[] = "Phone Notifications";
constexpr std::uint32_t kIconSize = 256;
constexpr float kScrollPixelsPerUnit = 180.0F;
constexpr float kDefaultPanelWidth = 1.5F;  // metres; the height follows from the 16:10 picture

// How wide the panel is, in metres. FRAME_NOTIFY_PANEL_WIDTH changes it (0.8 to 4) for anyone
// whose dashboard makes the default too big or too small.
float panel_width_meters() {
    const char* value = std::getenv("FRAME_NOTIFY_PANEL_WIDTH");
    if (value == nullptr || *value == '\0') return kDefaultPanelWidth;
    char* end = nullptr;
    const double parsed = std::strtod(value, &end);
    if (end == value || *end != '\0' || !(parsed >= 0.8 && parsed <= 4.0)) {
        std::cerr << "[Dashboard] Ignoring invalid FRAME_NOTIFY_PANEL_WIDTH=" << value
                  << "; using " << kDefaultPanelWidth << '\n';
        return kDefaultPanelWidth;
    }
    return static_cast<float>(parsed);
}

DashboardAction make_action(DashboardActionType type, std::string notification_id = {},
                            std::string argument = {}) {
    return DashboardAction{type, std::move(notification_id), std::move(argument)};
}

const char* overlay_error_name(vr::IVROverlay* api, vr::EVROverlayError error) {
    const char* name = api->GetOverlayErrorNameFromEnum(error);
    return name != nullptr ? name : "unknown overlay error";
}

}  // namespace

Dashboard::~Dashboard() {
    destroy();
}

bool Dashboard::create(vr::IVROverlay* overlay_api) {
    if (overlay_api == nullptr) {
        std::cerr << "[Dashboard] Cannot create overlay: IVROverlay is null\n";
        return false;
    }

    overlay_api_ = overlay_api;
    vr::VROverlayHandle_t main_handle = vr::k_ulOverlayHandleInvalid;
    vr::VROverlayHandle_t thumbnail_handle = vr::k_ulOverlayHandleInvalid;
    const auto create_error = overlay_api_->CreateDashboardOverlay(
        kOverlayKey, kFriendlyName, &main_handle, &thumbnail_handle);

    if (create_error != vr::VROverlayError_None) {
        std::cerr << "[Dashboard] CreateDashboardOverlay failed: "
                  << overlay_error_name(overlay_api_, create_error) << " ("
                  << static_cast<int>(create_error) << ")\n";
        overlay_api_ = nullptr;
        return false;
    }

    main_handle_ = main_handle;
    thumbnail_handle_ = thumbnail_handle;

    auto icon = ui::make_notification_icon(kIconSize);
    const auto texture_error = overlay_api_->SetOverlayRaw(
        thumbnail_handle, icon.data(), kIconSize, kIconSize, ui::kIconBytesPerPixel);
    if (texture_error != vr::VROverlayError_None) {
        std::cerr << "[Dashboard] SetOverlayRaw for thumbnail failed: "
                  << overlay_error_name(overlay_api_, texture_error) << " ("
                  << static_cast<int>(texture_error) << ")\n";
        destroy();
        return false;
    }

    // SteamVR reports the pointer as a position in the whole texture scaled by this, so it is the
    // texture's size: an event's position is then a pixel of the texture (see ui::ScrollTexture).
    const vr::HmdVector2_t mouse_scale{{static_cast<float>(ui::ScrollTexture::kWidth),
                                        static_cast<float>(ui::ScrollTexture::kHeight)}};
    const auto input_error =
        overlay_api_->SetOverlayInputMethod(main_handle, vr::VROverlayInputMethod_Mouse);
    const auto smooth_scroll_flag_error = overlay_api_->SetOverlayFlag(
        main_handle, vr::VROverlayFlags_SendVRSmoothScrollEvents, true);
    const auto click_stabilization_error = overlay_api_->SetOverlayFlag(
        main_handle, vr::VROverlayFlags_EnableClickStabilization, true);
    // The full SteamVR control bar retains the dashboard's grab/move affordance. The minimal
    // variant exposes controls but prevents repositioning on the Steam Frame runtime.
    const auto control_bar_error = overlay_api_->SetOverlayFlag(
        main_handle, vr::VROverlayFlags_EnableControlBar, true);
    const auto close_control_error = overlay_api_->SetOverlayFlag(
        main_handle, vr::VROverlayFlags_EnableControlBarClose, true);
    const auto mouse_scale_error = overlay_api_->SetOverlayMouseScale(main_handle, &mouse_scale);
    const float width_meters = panel_width_meters();
    const auto width_error = overlay_api_->SetOverlayWidthInMeters(main_handle, width_meters);
    if (input_error != vr::VROverlayError_None ||
        smooth_scroll_flag_error != vr::VROverlayError_None ||
        click_stabilization_error != vr::VROverlayError_None ||
        control_bar_error != vr::VROverlayError_None ||
        close_control_error != vr::VROverlayError_None ||
        mouse_scale_error != vr::VROverlayError_None || width_error != vr::VROverlayError_None) {
        auto setup_error = input_error;
        if (setup_error == vr::VROverlayError_None) setup_error = smooth_scroll_flag_error;
        if (setup_error == vr::VROverlayError_None) setup_error = click_stabilization_error;
        if (setup_error == vr::VROverlayError_None) setup_error = control_bar_error;
        if (setup_error == vr::VROverlayError_None) setup_error = close_control_error;
        if (setup_error == vr::VROverlayError_None) setup_error = mouse_scale_error;
        if (setup_error == vr::VROverlayError_None) setup_error = width_error;
        std::cerr << "[Dashboard] Main overlay configuration failed: "
                  << overlay_error_name(overlay_api_, setup_error) << " ("
                  << static_cast<int>(setup_error) << ")\n";
        destroy();
        return false;
    }

    if (!upload_content() || !update_scroll_view(0)) {
        destroy();
        return false;
    }
    uploaded_signature_ = current_signature();
    uploaded_screen_ = screen_;
    has_uploaded_ = true;
    last_tick_ = std::chrono::steady_clock::now();

    std::cout << "[Dashboard] CreateDashboardOverlay returned: VROverlayError_None\n"
              << "[Dashboard] Key: " << kOverlayKey << '\n'
              << "[Dashboard] Friendly name: " << kFriendlyName << '\n'
              << "[Dashboard] Main handle: " << main_handle_ << '\n'
              << "[Dashboard] Thumbnail handle: " << thumbnail_handle_ << '\n'
              << "[Dashboard] Panel width: " << width_meters << " m\n"
              << "[Dashboard] Thumbnail uploaded: " << kIconSize << 'x' << kIconSize
              << " RGBA\n"
              << "[Dashboard] Panel texture uploaded: " << ui::ScrollTexture::kWidth << 'x'
              << ui::ScrollTexture::kHeight << " RGBA (a window of the content); viewport "
              << ui::kHistoryViewWidth << 'x' << ui::kHistoryViewHeight << "\n";
    return true;
}

std::vector<DashboardAction> Dashboard::poll_events() {
    std::vector<DashboardAction> actions;
    if (overlay_api_ == nullptr || main_handle_ == vr::k_ulOverlayHandleInvalid) {
        return actions;
    }

    const bool skip_pointer_moves = std::exchange(texture_replaced_, false);
    vr::VREvent_t event{};
    while (overlay_api_->PollNextOverlayEvent(main_handle_, &event, sizeof(event))) {
        switch (event.eventType) {
        case vr::VREvent_OverlayShown:
            std::cout << "[Dashboard] Overlay shown\n";
            dashboard_visible_ = true;
            refresh_pending_ = true;  // relative times may have gone stale while hidden
            break;
        case vr::VREvent_OverlayHidden:
            std::cout << "[Dashboard] Overlay hidden\n";
            scroll_.cancel();
            if (dashboard_visible_) {
                // Only what was actually on screen counts as seen.
                if (screen_ == Screen::kNotifications) {
                    actions.push_back(make_action(DashboardActionType::MarkAllRead));
                }
                dashboard_visible_ = false;
            }
            // Next time the dashboard opens it shows the notifications again, unless a pairing is
            // still in progress and needs the user.
            if ((screen_ == Screen::kPhone && !ui::phone_state_is_pairing(phone_.state)) ||
                screen_ == Screen::kSettings) {
                show_screen(Screen::kNotifications);
            }
            break;
        case vr::VREvent_MouseMove: {
            // Positions queued while the texture was being replaced mix the old crop with the new
            // window; skip them rather than let a drag jump.
            if (skip_pointer_moves) break;
            const ui::PanelPoint point = pointer_on_panel(event.data.mouse.x, event.data.mouse.y);
            mouse_x_ = point.x;
            mouse_y_ = point.y;
            scroll_.move(mouse_y_);  // only acts while the button is held
            break;
        }
        case vr::VREvent_MouseButtonDown:
            if (event.data.mouse.button != vr::VRMouseButton_Left) break;
            // Whether this is a tap or the start of a drag is only known once it is released.
            press_x_ = mouse_x_;
            press_y_ = mouse_y_;
            // A click that lands somewhere else than the last move reported would explain a
            // mismatch between where people point and what is hit; say so in the log.
            if (const ui::PanelPoint down = pointer_on_panel(event.data.mouse.x, event.data.mouse.y);
                !skip_pointer_moves && (std::fabs(down.x - mouse_x_) > 3.0F || std::fabs(down.y - mouse_y_) > 3.0F)) {
                std::cout << "[Dashboard] Button down reported x=" << down.x << " y=" << down.y
                          << " but the last move was x=" << mouse_x_ << " y=" << mouse_y_ << '\n';
            }
            scroll_.press(mouse_y_);
            break;
        case vr::VREvent_MouseButtonUp:
            if (event.data.mouse.button != vr::VRMouseButton_Left) break;
            if (scroll_.release()) handle_click(press_x_, press_y_, actions);
            break;
        case vr::VREvent_FocusLeave:
            scroll_.cancel();  // the pointer left the panel; its button-up may never arrive
            break;
        case vr::VREvent_OverlayClosed:
            std::cout << "[Dashboard] Close control selected\n";
            actions.push_back(make_action(DashboardActionType::Exit));
            break;
        case vr::VREvent_ScrollDiscrete:
        case vr::VREvent_ScrollSmooth:
            scroll_.scroll_by(-event.data.scroll.ydelta * kScrollPixelsPerUnit);
            break;
        case vr::VREvent_Notification_Shown:
            std::cout << "[Event] Notification_Shown id="
                      << event.data.notification.notificationId << " userValue="
                      << event.data.notification.ulUserValue << '\n';
            break;
        case vr::VREvent_Notification_Hidden:
            std::cout << "[Event] Notification_Hidden id="
                      << event.data.notification.notificationId << " userValue="
                      << event.data.notification.ulUserValue << '\n';
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

    // Re-lay out when a card was expanded or the dashboard was just shown, and once a minute while
    // it is open, so "3 min ago" keeps up with the clock; the phone screen counts seconds instead,
    // for the pairing countdown. Nothing is uploaded unless it shows.
    if (dashboard_visible_) {
        const auto now_time = std::chrono::system_clock::now().time_since_epoch();
        const bool minute_passed =
            std::chrono::duration_cast<std::chrono::minutes>(now_time).count() != rendered_minute_;
        const bool second_passed = screen_ == Screen::kPhone &&
                                   std::chrono::duration_cast<std::chrono::seconds>(now_time).count() !=
                                       rendered_second_;
        if (refresh_pending_ || minute_passed || second_passed) refresh();
    }
    refresh_pending_ = false;

    const auto now = std::chrono::steady_clock::now();
    scroll_.tick(std::chrono::duration<float>(now - last_tick_).count());
    last_tick_ = now;

    const int next_offset = static_cast<int>(std::lround(scroll_.position()));
    if (next_offset != uploaded_scroll_offset_) {
        if (update_scroll_view(next_offset)) {
            uploaded_scroll_offset_ = next_offset;
        } else {
            scroll_.jump_to(static_cast<float>(uploaded_scroll_offset_));  // do not retry every poll
        }
    }
    return actions;
}

void Dashboard::handle_click(float x, float y, std::vector<DashboardAction>& actions) {
    std::cout << "[Dashboard] Select x=" << x << " y=" << y << " scroll=" << uploaded_scroll_offset_
              << '\n';
    if (screen_ == Screen::kPhone) {
        handle_phone_click(x, y, actions);
        return;
    }
    if (screen_ == Screen::kSettings) {
        handle_settings_click(x, y, actions);
        return;
    }
    const auto target = view_.hit_test(static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)),
                                       uploaded_scroll_offset_);
    if (!target) {
        std::cout << "[Dashboard] Selection did not hit a control\n";
    } else if (target->kind == ui::HistoryHitKind::kClearAll) {
        std::cout << "[Dashboard] Clear all selected\n";
        actions.push_back(make_action(DashboardActionType::ClearAll));
    } else if (target->kind == ui::HistoryHitKind::kDismiss) {
        std::cout << "[Dashboard] Clear selected index=" << target->notification_index << '\n';
        actions.push_back(make_action(DashboardActionType::Dismiss,
                                      view_.notifications()[target->notification_index].id));
    } else if (target->kind == ui::HistoryHitKind::kPhoneChip) {
        std::cout << "[Dashboard] Phone status selected\n";
        show_screen(Screen::kPhone);
    } else if (target->kind == ui::HistoryHitKind::kSettings) {
        std::cout << "[Dashboard] Settings selected\n";
        show_screen(Screen::kSettings);
    } else if (target->kind == ui::HistoryHitKind::kPairPrompt) {
        std::cout << "[Dashboard] Pair an iPhone selected\n";
        actions.push_back(make_action(DashboardActionType::PairStart));
        show_screen(Screen::kPhone);
    } else {
        // A tap on the card body expands a long message, or collapses it again.
        const std::string& id = view_.notifications()[target->notification_index].id;
        expanded_id_ = expanded_id_ == id ? std::string() : id;
        std::cout << "[Dashboard] Card " << (expanded_id_.empty() ? "collapsed" : "expanded")
                  << " index=" << target->notification_index << '\n';
        refresh_pending_ = true;
    }
}

void Dashboard::handle_phone_click(float x, float y, std::vector<DashboardAction>& actions) {
    const auto hit = phone_view_.hit_test(static_cast<int>(std::lround(x)),
                                          static_cast<int>(std::lround(y)), uploaded_scroll_offset_);
    if (!hit) {
        std::cout << "[Dashboard] Selection did not hit a control\n";
        return;
    }
    std::cout << "[Dashboard] Phone screen button selected: " << static_cast<int>(hit->button) << '\n';
    switch (hit->button) {
    case ui::PhoneButton::kStartPairing:
        actions.push_back(make_action(DashboardActionType::PairStart));
        break;
    case ui::PhoneButton::kCancel:
        actions.push_back(make_action(DashboardActionType::PairCancel));
        show_screen(Screen::kNotifications);
        break;
    case ui::PhoneButton::kConfirm:
        actions.push_back(make_action(DashboardActionType::PairConfirm));
        break;
    case ui::PhoneButton::kReject:
        actions.push_back(make_action(DashboardActionType::PairReject));
        break;
    case ui::PhoneButton::kRemoveConflict:
        actions.push_back(make_action(DashboardActionType::RemoveConflict, {}, hit->argument));
        break;
    case ui::PhoneButton::kContinueAnyway:
        actions.push_back(make_action(DashboardActionType::PairAnyway));
        break;
    case ui::PhoneButton::kDismiss:
        actions.push_back(make_action(DashboardActionType::PairDismiss));
        show_screen(Screen::kNotifications);
        break;
    case ui::PhoneButton::kClose:
        show_screen(Screen::kNotifications);
        break;
    case ui::PhoneButton::kRetry:
        actions.push_back(make_action(DashboardActionType::RetryBluetooth));
        break;
    case ui::PhoneButton::kPowerOn:
        actions.push_back(make_action(DashboardActionType::PowerOnBluetooth));
        break;
    case ui::PhoneButton::kForget:
        confirm_forget_ = true;
        refresh();
        break;
    case ui::PhoneButton::kForgetConfirmed:
        actions.push_back(make_action(DashboardActionType::ForgetPhone));
        show_screen(Screen::kNotifications);
        break;
    case ui::PhoneButton::kForgetCancelled:
        confirm_forget_ = false;
        refresh();
        break;
    case ui::PhoneButton::kToggleAutostart:
        break;   // a settings button; the phone screens have none
    }
}

void Dashboard::handle_settings_click(float x, float y, std::vector<DashboardAction>& actions) {
    const auto hit = settings_view_.hit_test(static_cast<int>(std::lround(x)),
                                             static_cast<int>(std::lround(y)), uploaded_scroll_offset_);
    if (!hit) {
        std::cout << "[Dashboard] Selection did not hit a control\n";
        return;
    }
    if (hit->button == ui::PhoneButton::kToggleAutostart) {
        std::cout << "[Dashboard] Autostart toggle selected\n";
        actions.push_back(make_action(DashboardActionType::ToggleAutostart));
    } else if (hit->button == ui::PhoneButton::kClose) {
        show_screen(Screen::kNotifications);
    }
}

void Dashboard::show_screen(Screen screen) {
    confirm_forget_ = false;
    scroll_.cancel();
    if (screen == screen_) {
        refresh();
        return;
    }
    if (screen_ == Screen::kNotifications) notifications_scroll_ = uploaded_scroll_offset_;
    screen_ = screen;
    if (screen_ != Screen::kNotifications) {
        scroll_.reset();
        uploaded_scroll_offset_ = 0;
    } else {
        // Back to where the list was, as far as it still reaches.
        scroll_.set_maximum(static_cast<float>(view_.maximum_scroll_offset()));
        scroll_.jump_to(static_cast<float>(notifications_scroll_));
        uploaded_scroll_offset_ = std::clamp(notifications_scroll_, 0, view_.maximum_scroll_offset());
    }
    refresh_pending_ = false;
    refresh();
}

void Dashboard::update_thumbnail(int unread) {
    if (overlay_api_ == nullptr || thumbnail_handle_ == vr::k_ulOverlayHandleInvalid) return;
    auto icon = ui::make_notification_icon(kIconSize, unread);
    const auto error = overlay_api_->SetOverlayRaw(
        thumbnail_handle_, icon.data(), kIconSize, kIconSize, ui::kIconBytesPerPixel);
    if (error != vr::VROverlayError_None) {
        std::cerr << "[Dashboard] Updating the tile badge failed: "
                  << overlay_error_name(overlay_api_, error) << " (" << static_cast<int>(error)
                  << ")\n";
        return;
    }
    thumbnail_unread_ = unread;
}

ui::HistoryContext Dashboard::make_context() {
    ui::HistoryContext context;
    context.now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch())
                      .count();
    context.utc_offset_seconds = ui::local_utc_offset_seconds(context.now);
    context.expanded_id = expanded_id_;
    context.phone = phone_with_elapsed_time();
    rendered_minute_ = context.now / 60;
    return context;
}

ui::PhoneInfo Dashboard::phone_with_elapsed_time() const {
    ui::PhoneInfo phone = phone_;
    phone.seconds_in_state = static_cast<int>(std::max<std::int64_t>(
        0, std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
                                                            phone_changed_at_)
               .count()));
    return phone;
}

std::uint64_t Dashboard::current_signature() const noexcept {
    return screen_ == Screen::kNotifications ? view_.signature() : card_view().signature();
}

bool Dashboard::upload_content() {
    // Render the whole screen once, then upload the part of it around the current scroll position.
    const bool card_screen = screen_ != Screen::kNotifications;
    content_ = card_screen ? card_view().render() : view_.render_content();
    content_rows_ = card_screen ? card_view().content_height() : view_.content_height();
    return upload_window(uploaded_scroll_offset_);
}

ui::PanelPoint Dashboard::pointer_on_panel(float mouse_x, float mouse_y) const {
    return texture_.panel_point(mouse_x, mouse_y, uploaded_scroll_offset_);
}

bool Dashboard::upload_window(int scroll_offset) {
    // Moving on from where the picture was last shown: keep the window's room on the far side.
    const int direction = scroll_offset > uploaded_scroll_offset_ ? 1 : scroll_offset < uploaded_scroll_offset_ ? -1 : 0;
    const int previous_top = texture_.top();
    texture_.place(scroll_offset, content_rows_, direction);
    if (texture_.top() != previous_top) texture_replaced_ = true;
    auto pixels = texture_.build(content_, content_rows_);
    const auto error = overlay_api_->SetOverlayRaw(
        main_handle_, pixels.data(),
        static_cast<std::uint32_t>(ui::ScrollTexture::kWidth),
        static_cast<std::uint32_t>(ui::ScrollTexture::kHeight),
        static_cast<std::uint32_t>(ui::ScrollTexture::kBytesPerPixel));
    if (error != vr::VROverlayError_None) {
        std::cerr << "[Dashboard] SetOverlayRaw for history content failed: "
                  << overlay_error_name(overlay_api_, error) << " (" << static_cast<int>(error)
                  << ")\n";
        ++overlay_failures_;
        return false;
    }
    return true;
}

bool Dashboard::update_scroll_view(int scroll_offset) {
    // Scrolling moves the crop inside the texture; the texture is only replaced when the crop would
    // leave it, and always keeps the viewport's shape (see ui::ScrollTexture).
    if (!texture_.covers(scroll_offset) && !upload_window(scroll_offset)) return false;
    const ui::TextureBounds shown = texture_.bounds(scroll_offset);
    const vr::VRTextureBounds_t bounds{shown.u_min, shown.v_min, shown.u_max, shown.v_max};
    const auto error = overlay_api_->SetOverlayTextureBounds(main_handle_, &bounds);
    if (error != vr::VROverlayError_None) {
        std::cerr << "[Dashboard] SetOverlayTextureBounds failed: "
                  << overlay_error_name(overlay_api_, error) << " (" << static_cast<int>(error)
                  << ")\n";
        ++overlay_failures_;
        return false;
    }
    overlay_failures_ = 0;
    return true;
}

bool Dashboard::set_history(std::vector<ui::HistoryNotification> notifications, bool reset_scroll) {
    // Forget an expanded card that is no longer in the list.
    if (!expanded_id_.empty() &&
        std::none_of(notifications.begin(), notifications.end(),
                     [this](const ui::HistoryNotification& notification) {
                         return notification.id == expanded_id_;
                     })) {
        expanded_id_.clear();
    }
    view_.set(std::move(notifications), make_context());
    const auto unread = static_cast<int>(std::count_if(
        view_.notifications().begin(), view_.notifications().end(),
        [](const ui::HistoryNotification& notification) { return !notification.read; }));
    if (unread != thumbnail_unread_) update_thumbnail(unread);
    if (screen_ != Screen::kNotifications) {
        // The list is only laid out for later; the card screen stays as it is.
        if (reset_scroll) notifications_scroll_ = 0;
        return present(false);
    }
    return present(reset_scroll);
}

bool Dashboard::present(bool reset_scroll) {
    const bool phone_screen = screen_ != Screen::kNotifications;   // a card screen, phone or settings
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    rendered_second_ = now;
    rendered_minute_ = now / 60;
    if (screen_ == Screen::kPhone) phone_view_.set(phone_with_elapsed_time(), confirm_forget_);
    if (screen_ == Screen::kSettings) settings_view_.set_screen(ui::describe_settings_screen(settings_));

    const int maximum_scroll =
        phone_screen ? card_view().maximum_scroll_offset() : view_.maximum_scroll_offset();
    if (reset_scroll) {
        scroll_.reset();
        uploaded_scroll_offset_ = 0;
    }
    scroll_.set_maximum(static_cast<float>(maximum_scroll));
    uploaded_scroll_offset_ = std::clamp(uploaded_scroll_offset_, 0, maximum_scroll);

    // The image is only uploaded when something on it changed; repeating an identical upload
    // would cost time and risk a flicker for nothing.
    const std::uint64_t signature = current_signature();
    if (has_uploaded_ && uploaded_screen_ == screen_ && signature == uploaded_signature_) {
        return update_scroll_view(uploaded_scroll_offset_);
    }
    if (!upload_content() || !update_scroll_view(uploaded_scroll_offset_)) return false;
    uploaded_signature_ = signature;
    uploaded_screen_ = screen_;
    has_uploaded_ = true;
    return true;
}

bool Dashboard::set_phone(ui::PhoneInfo phone) {
    const bool state_changed = phone.state != phone_.state;
    if (state_changed || phone.fields != phone_.fields) phone_changed_at_ = std::chrono::steady_clock::now();
    phone_ = std::move(phone);
    phone_.seconds_in_state = 0;  // always worked out from phone_changed_at_
    if (state_changed) {
        std::cout << "[Phone] State: " << phone_.state << '\n';
        // A step that waits for the user comes forward by itself, also while the dashboard is
        // closed, so the code is there when they open it.
        if (ui::phone_state_needs_attention(phone_.state) && screen_ != Screen::kPhone) {
            show_screen(Screen::kPhone);
            return true;
        }
        // A new screen starts at the top, wherever the last one had been dragged to.
        if (screen_ == Screen::kPhone) {
            scroll_.reset();
            uploaded_scroll_offset_ = 0;
        }
    }
    return refresh();
}

bool Dashboard::set_settings(ui::SettingsInfo settings) {
    settings_ = std::move(settings);
    return screen_ == Screen::kSettings ? present(false) : true;
}

bool Dashboard::refresh() {
    if (screen_ != Screen::kNotifications) return present(false);
    return set_history(view_.notifications(), false);
}

void Dashboard::destroy() noexcept {
    if (overlay_api_ == nullptr) {
        return;
    }

    if (thumbnail_handle_ != vr::k_ulOverlayHandleInvalid) {
        overlay_api_->DestroyOverlay(thumbnail_handle_);
    }
    if (main_handle_ != vr::k_ulOverlayHandleInvalid) {
        overlay_api_->DestroyOverlay(main_handle_);
    }

    thumbnail_handle_ = vr::k_ulOverlayHandleInvalid;
    main_handle_ = vr::k_ulOverlayHandleInvalid;
    overlay_api_ = nullptr;
    dashboard_visible_ = false;
}

}  // namespace frame_notify::openvr
