#include "ui/phone_view.h"

#include "ui/fingerprint.h"
#include "ui/history_view.h"
#include "ui/icon.h"
#include "ui/theme.h"
#include "ui/typography.h"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

namespace frame_notify::ui {
using namespace theme;

namespace {

// ---- Geometry (pixels) ---------------------------------------------------------------------
constexpr float kPanelWidth = static_cast<float>(kHistoryViewWidth);
constexpr float kCardLeft = 110.0F;
constexpr float kCardRight = 1170.0F;
constexpr float kCardRadius = 40.0F;
constexpr float kCardTop = 34.0F;      // the card hangs from the top of the panel, not its middle
constexpr float kPaddingTop = 40.0F;
constexpr float kPaddingBottom = 40.0F;
constexpr float kIconDiameter = 100.0F;
// Empty room kept below the card, so the screen can always be dragged up to bring its buttons
// higher, wherever SteamVR happens to put the panel.
constexpr int kReachRoom = 224;
constexpr float kTitleWidth = 880.0F;
constexpr float kBodyWidth = 860.0F;
constexpr float kStepBlockWidth = 820.0F;
constexpr float kBodyLine = 34.0F;
constexpr float kButtonHeight = 64.0F;
constexpr float kButtonGap = 20.0F;
constexpr float kButtonMinimumWidth = 220.0F;
constexpr float kButtonMaximumRow = 960.0F;
constexpr float kButtonHitPadding = 6.0F;
constexpr std::size_t kMaximumNameLabel = 20;   // code points of a phone's name on a button

constexpr TextStyle kTitleFont{42.0F, FontWeight::kSemiBold, 0.0F};
constexpr TextStyle kBodyFont{24.0F, FontWeight::kRegular, 0.0F};
constexpr TextStyle kStepNumberFont{19.0F, FontWeight::kSemiBold, 0.0F};
constexpr TextStyle kCodeFont{96.0F, FontWeight::kSemiBold, 10.0F};
constexpr TextStyle kFooterFont{21.0F, FontWeight::kRegular, 0.0F};
constexpr TextStyle kButtonFont{24.0F, FontWeight::kSemiBold, 0.0F};

constexpr char kChevron[] = "\xE2\x80\xBA";   // a single right-pointing angle quote

int parse_seconds(const std::string& text) {
    int value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size() ? value : 0;
}

// At most `limit` characters of a phone's name, with an ellipsis when it was longer.
std::string shorten(const std::string& name, std::size_t limit) {
    std::size_t count = 0;
    for (std::size_t offset = 0; offset < name.size(); ++offset) {
        if ((static_cast<unsigned char>(name[offset]) & 0xC0U) == 0x80U) continue;   // continuation
        if (count == limit) return name.substr(0, offset) + "\xE2\x80\xA6";
        ++count;
    }
    return name;
}

// “Frame”, as the iPhone lists it: with curly quotes.
std::string quoted_frame() {
    return std::string("\xE2\x80\x9C") + "Frame" + "\xE2\x80\x9D";
}

// Countdowns tick in steps of five seconds, so the picture (which has to be uploaded again in full
// whenever it changes) is not replaced every second. Never more than what is really left.
int coarse_seconds(int seconds) {
    return seconds >= 5 ? seconds - seconds % 5 : std::max(0, seconds);
}

std::string grouped_code(const std::string& code) {
    if (code.size() == 6U) return code.substr(0, 3) + " " + code.substr(3);
    return code;
}

PhoneScreenButton button(PhoneButton id, std::string label, ButtonStyle style = ButtonStyle::kSecondary,
                         std::string argument = {}) {
    return {id, std::move(label), std::move(argument), style};
}

// The steps that turn on notification sharing, which several screens point the user to.
std::vector<std::string> sharing_steps() {
    return {std::string("On your iPhone, open Settings ") + kChevron + " Bluetooth.",
            "Tap the (i) button next to Frame.", "Turn on Share System Notifications."};
}

// Phone-shaped icon: a rounded outline with a speaker slot and a home bar.
void draw_phone(Canvas& canvas, float x, float y, Color color) {
    canvas.stroke_rounded_rect(x - 20.0F, y - 33.0F, x + 20.0F, y + 33.0F, 10.0F, 4.5F, color);
    canvas.fill_rounded_rect(x - 7.0F, y - 24.0F, x + 7.0F, y - 21.0F, 1.5F, color);
    canvas.fill_rounded_rect(x - 9.0F, y + 23.0F, x + 9.0F, y + 26.0F, 1.5F, color);
}

void draw_bluetooth(Canvas& canvas, float x, float y, Color color) {
    const Point points[] = {{-15, -15}, {15, 15}, {0, 30}, {0, -30}, {15, -15}, {-15, 15}};
    Path path;
    for (std::size_t index = 0; index + 1U < std::size(points); ++index) {
        path.add_capsule({x + points[index].x, y + points[index].y},
                         {x + points[index + 1U].x, y + points[index + 1U].y}, 2.7F);
    }
    canvas.fill_path(path, color);
}

void draw_check(Canvas& canvas, float x, float y, Color color) {
    Path path;
    path.add_capsule({x - 20.0F, y + 3.0F}, {x - 6.0F, y + 17.0F}, 4.4F);
    path.add_capsule({x - 6.0F, y + 17.0F}, {x + 22.0F, y - 14.0F}, 4.4F);
    canvas.fill_path(path, color);
}

void draw_warning(Canvas& canvas, float x, float y, Color color) {
    Path path;
    path.add_capsule({x, y - 22.0F}, {x, y + 4.0F}, 4.4F);
    canvas.fill_path(path, color);
    canvas.fill_circle(x, y + 20.0F, 5.0F, color);
}

}  // namespace

std::string format_countdown(int seconds) {
    seconds = std::max(0, seconds);
    const std::string remainder = std::to_string(seconds % 60);
    return std::to_string(seconds / 60) + ":" + (remainder.size() < 2U ? "0" : "") + remainder;
}

bool phone_state_can_forget(const std::string& state) {
    return state == "connecting" || state == "connected" || state == "needs_repair";
}

PhoneScreen describe_phone_screen(const PhoneInfo& info, bool confirm_forget) {
    const std::string& state = info.state;
    const std::string phone = info.field("phone").empty() ? std::string("your iPhone") : info.field("phone");
    const std::string detail = info.field("detail");
    PhoneScreen screen;

    if (confirm_forget && phone_state_can_forget(state)) {
        screen.title = "Forget " + phone + "?";
        screen.body = {"Frame Notify will stop listening to it and remove its pairing from this "
                       "Frame. You can pair it again at any time."};
        screen.tone = Tone::kWarning;
        screen.icon = PhoneIcon::kWarning;
        screen.buttons = {button(PhoneButton::kForgetCancelled, "Keep it", ButtonStyle::kPrimary),
                          button(PhoneButton::kForgetConfirmed, "Forget it", ButtonStyle::kDanger)};
        return screen;
    }

    if (state == "unpaired") {
        screen.title = "No iPhone paired";
        screen.body = {"Pair your iPhone to see its notifications here."};
        screen.icon = PhoneIcon::kPhone;
        screen.buttons = {button(PhoneButton::kStartPairing, "Pair an iPhone", ButtonStyle::kPrimary),
                          button(PhoneButton::kClose, "Close")};
    } else if (state == "connecting") {
        screen.title = "Looking for " + phone;
        screen.body = {"Keep your iPhone unlocked and close by. If it doesn't connect, switch its "
                       "Bluetooth off and on."};
        if (detail == "no_gatt" || detail == "no_ancs") {
            screen.body.push_back("The iPhone is connected but isn't offering notifications yet.");
        }
        screen.tone = Tone::kWarning;
        screen.icon = PhoneIcon::kBluetooth;
        screen.buttons = {button(PhoneButton::kStartPairing, "Pair again"),
                          button(PhoneButton::kForget, "Forget this phone"),
                          button(PhoneButton::kClose, "Close", ButtonStyle::kPrimary)};
    } else if (state == "connected") {
        screen.title = phone + " is connected";
        screen.body = {"Notifications from your iPhone will appear here.",
                       std::string("Nothing showing up? The iPhone only sends them once Share System Notifications "
                                   "is on for Frame: Settings ") + kChevron + " Bluetooth " + kChevron +
                           " (i) next to Frame."};
        screen.tone = Tone::kGood;
        screen.icon = PhoneIcon::kCheck;
        screen.buttons = {button(PhoneButton::kClose, "Close", ButtonStyle::kPrimary),
                          button(PhoneButton::kStartPairing, "Pair a different phone"),
                          button(PhoneButton::kForget, "Forget this phone")};
    } else if (state == "needs_repair") {
        const std::string reason = info.field("reason");
        screen.tone = Tone::kWarning;
        screen.icon = PhoneIcon::kWarning;
        if (reason == "not_allowed") {
            screen.title = "Allow notifications on your iPhone";
            screen.body = {"Your iPhone hasn't allowed Frame to see its notifications yet."};
            screen.steps = sharing_steps();
            screen.footer = "Frame Notify keeps trying in the background.";
            screen.buttons = {button(PhoneButton::kRetry, "Try again", ButtonStyle::kPrimary),
                              button(PhoneButton::kStartPairing, "Pair again"),
                              button(PhoneButton::kClose, "Close")};
        } else if (reason == "not_paired") {
            screen.title = phone + " isn't paired any more";
            screen.body = {"Its pairing was removed from Bluetooth. Pair it again to get notifications."};
            screen.buttons = {button(PhoneButton::kStartPairing, "Pair again", ButtonStyle::kPrimary),
                              button(PhoneButton::kForget, "Forget this phone"),
                              button(PhoneButton::kClose, "Close")};
        } else {
            screen.title = phone + " isn't sharing notifications";
            screen.body = {"The iPhone is paired but isn't offering notifications. This usually "
                           "means it paired over classic Bluetooth."};
            screen.steps = {std::string("On your iPhone, open Settings ") + kChevron +
                                " Bluetooth and tap the (i) next to Frame.",
                            "Choose Forget This Device.", "Then pair again here."};
            screen.buttons = {button(PhoneButton::kStartPairing, "Pair again", ButtonStyle::kPrimary),
                              button(PhoneButton::kRetry, "Try again"),
                              button(PhoneButton::kClose, "Close")};
        }
    } else if (state == "pair_conflict") {
        screen.title = "Is your iPhone already paired?";
        screen.body = {"These phones are already paired with the Frame. If your iPhone is one of "
                       "them, remove it here first; otherwise it stays on classic Bluetooth, which "
                       "can't carry notifications.",
                       "Afterwards, also tap the (i) next to Frame on your iPhone and choose Forget "
                       "This Device."};
        screen.tone = Tone::kActive;
        screen.icon = PhoneIcon::kPhone;
        for (int index = 1; index <= 3; ++index) {
            const std::string name = info.field("name" + std::to_string(index));
            const std::string address = info.field("address" + std::to_string(index));
            if (name.empty() || address.empty()) continue;
            screen.buttons.push_back(button(PhoneButton::kRemoveConflict,
                                            "Remove " + shorten(name, kMaximumNameLabel),
                                            ButtonStyle::kDanger, address));
        }
        screen.buttons.push_back(button(PhoneButton::kContinueAnyway, "My iPhone isn't listed",
                                        ButtonStyle::kPrimary));
        screen.buttons.push_back(button(PhoneButton::kCancel, "Cancel"));
    } else if (state == "pair_open") {
        screen.title = "Pair your iPhone";
        if (detail == "rejected") {
            screen.body = {"You said the codes differ. Tap " + quoted_frame() + " on the iPhone to try again."};
        } else if (detail == "expired") {
            screen.body = {"The code wasn't confirmed in time. Tap " + quoted_frame() + " on the iPhone to try again."};
        }
        screen.steps = {std::string("On your iPhone, open Settings ") + kChevron + " Bluetooth.",
                        "Under Other Devices, tap " + quoted_frame() + ".",
                        "If it isn't listed, switch the iPhone's Bluetooth off and on."};
        const int left = coarse_seconds(parse_seconds(info.field("seconds")) - info.seconds_in_state);
        screen.footer = left > 0 ? "Waiting for your iPhone \xC2\xB7 " + format_countdown(left) + " left"
                                 : "Closing the pairing window\xE2\x80\xA6";
        screen.tone = Tone::kActive;
        screen.icon = PhoneIcon::kBluetooth;
        screen.buttons = {button(PhoneButton::kCancel, "Cancel")};
    } else if (state == "pair_confirm") {
        const std::string code = info.field("code");
        if (code.empty()) {
            screen.title = "Allow this device to pair?";
            screen.body = {phone + " wants to pair with the Frame."};
        } else {
            screen.title = "Is this the code on your iPhone?";
            screen.code = grouped_code(code);
        }
        const int left = coarse_seconds(parse_seconds(info.field("seconds")) - info.seconds_in_state);
        screen.footer = "Answer within " + std::to_string(left) + " seconds";
        screen.tone = Tone::kActive;
        screen.icon = PhoneIcon::kPhone;
        screen.buttons = {button(PhoneButton::kConfirm, code.empty() ? "Allow" : "Yes, it matches",
                                 ButtonStyle::kPrimary),
                          button(PhoneButton::kReject, code.empty() ? "Don't allow" : "No, it doesn't")};
    } else if (state == "pair_verify") {
        screen.title = "Checking the connection\xE2\x80\xA6";
        screen.body = {"Paired with " + phone + ". Making sure notifications can reach the Frame."};
        screen.tone = Tone::kActive;
        screen.icon = PhoneIcon::kBluetooth;
    } else if (state == "pair_done") {
        screen.icon = PhoneIcon::kCheck;
        screen.buttons = {button(PhoneButton::kDismiss, "Done", ButtonStyle::kPrimary)};
        if (info.field("ancs") == "yes") {
            // Paired and visible to the Frame, but the iPhone sends nothing until it is allowed to.
            screen.title = "Paired with " + phone;
            screen.body = {"One last step: the iPhone only sends notifications once you allow it."};
            screen.steps = sharing_steps();
            screen.footer = "No switch yet? Turn the iPhone's Bluetooth off and on.";
            screen.tone = Tone::kGood;
        } else {
            screen.title = "Paired, one more step";
            screen.body = {"The iPhone isn't sharing notifications yet."};
            screen.steps = sharing_steps();
            screen.tone = Tone::kWarning;
        }
    } else if (state == "pair_failed") {
        const std::string reason = info.field("reason");
        screen.tone = Tone::kBad;
        screen.icon = PhoneIcon::kWarning;
        if (reason == "timeout") {
            screen.title = "Nothing paired in time";
            screen.body = {"The pairing window closed after five minutes."};
        } else if (reason == "classic_only") {
            screen.title = "That pairing can't carry notifications";
            screen.body = {phone + " paired over classic Bluetooth, so it was removed again."};
            screen.steps = {"On your iPhone, tap the (i) next to Frame and choose Forget This Device.",
                            "Then try again, starting from " + quoted_frame() + " under Other Devices."};
        } else if (reason == "advertising") {
            screen.title = "Couldn't make the Frame visible";
            screen.body = {detail.empty() ? std::string("Bluetooth refused to advertise.")
                                          : "Bluetooth refused to advertise: " + detail};
        } else if (reason == "agent") {
            screen.title = "Bluetooth stopped the pairing";
            screen.body = {"Another program took over Bluetooth pairing. Try again."};
        } else {
            screen.title = "Pairing didn't work";
            screen.body = {detail.empty() ? std::string("Something went wrong.") : detail};
        }
        screen.buttons = {button(PhoneButton::kStartPairing, "Try again", ButtonStyle::kPrimary),
                          button(PhoneButton::kDismiss, "Close")};
    } else if (state == "no_bluetooth") {
        const std::string reason = info.field("reason");
        screen.tone = Tone::kBad;
        screen.icon = PhoneIcon::kBluetooth;
        if (reason == "powered_off") {
            screen.title = "Bluetooth is turned off";
            screen.body = {"Turn it on to connect your iPhone."};
            screen.buttons = {button(PhoneButton::kPowerOn, "Turn on Bluetooth", ButtonStyle::kPrimary),
                              button(PhoneButton::kClose, "Close")};
        } else {
            if (reason == "no_adapter") {
                screen.title = "No Bluetooth adapter found";
                screen.body = {"This Frame doesn't report a Bluetooth adapter that Frame Notify can use."};
            } else if (reason == "no_le") {
                screen.title = "Bluetooth LE isn't available";
                screen.body = {"Notifications from an iPhone need Bluetooth Low Energy advertising, "
                               "which this adapter doesn't provide."};
            } else if (reason == "ambiguous") {
                screen.title = "More than one Bluetooth adapter";
                screen.body = {"Frame Notify can't tell which one to use."};
            } else {
                screen.title = "Bluetooth isn't available";
                screen.body = {"The Bluetooth service isn't responding."};
            }
            if (!detail.empty()) screen.body.push_back(detail);
            screen.buttons = {button(PhoneButton::kRetry, "Try again", ButtonStyle::kPrimary),
                              button(PhoneButton::kClose, "Close")};
        }
    } else if (state == "helper_unavailable") {
        screen.title = "The Bluetooth helper can't run";
        screen.body = {info.field("message").empty() ? std::string("It stopped and could not be started.")
                                                     : info.field("message"),
                       "Phone notifications need Python 3 with the dbus-python and PyGObject modules."};
        screen.tone = Tone::kBad;
        screen.icon = PhoneIcon::kWarning;
        screen.buttons = {button(PhoneButton::kRetry, "Try again", ButtonStyle::kPrimary),
                          button(PhoneButton::kClose, "Close")};
    } else if (state == "helper_restarting") {
        screen.title = "Restarting Bluetooth\xE2\x80\xA6";
        screen.body = {"The Bluetooth helper stopped unexpectedly and is starting again."};
        screen.tone = Tone::kWarning;
        screen.icon = PhoneIcon::kBluetooth;
        screen.buttons = {button(PhoneButton::kClose, "Close")};
    } else if (state == "starting") {
        screen.title = "Starting Bluetooth\xE2\x80\xA6";
        screen.body = {"Setting things up."};
        screen.icon = PhoneIcon::kBluetooth;
        screen.buttons = {button(PhoneButton::kClose, "Close")};
    } else {
        screen.title = "Bluetooth";
        screen.body = {"Status: " + state};
        screen.icon = PhoneIcon::kBluetooth;
        screen.buttons = {button(PhoneButton::kClose, "Close")};
    }
    return screen;
}

PhoneScreen describe_settings_screen(const SettingsInfo& info) {
    PhoneScreen screen;
    screen.title = "Settings";
    screen.icon = PhoneIcon::kGear;
    screen.tone = info.autostart_enabled ? Tone::kGood : Tone::kNeutral;
    if (info.autostart_enabled) {
        screen.body = {"Start automatically: on",
                       "Frame Notify starts by itself when the Frame starts and waits in the background "
                       "until SteamVR is running, so notifications are collected before you even put the "
                       "headset on."};
        if (info.autostart_method == "desktop") {
            screen.body.push_back("This uses a desktop autostart entry, because the Frame has no systemd "
                                  "user service to use.");
        }
        screen.buttons = {button(PhoneButton::kClose, "Close", ButtonStyle::kPrimary),
                          button(PhoneButton::kToggleAutostart, "Turn off autostart")};
    } else {
        screen.body = {"Start automatically: off",
                       "Frame Notify only runs until you stop it or the Frame restarts. Turn this on to "
                       "have it start with the Frame and wait in the background for SteamVR."};
        screen.buttons = {button(PhoneButton::kToggleAutostart, "Turn on autostart", ButtonStyle::kPrimary),
                          button(PhoneButton::kClose, "Close")};
    }
    if (!info.message.empty()) {
        screen.body.push_back(info.message);
        screen.tone = Tone::kWarning;
    }
    if (!info.version.empty()) screen.footer = "Frame Notify " + info.version;
    return screen;
}

void PhoneView::set(const PhoneInfo& info, bool confirm_forget) {
    set_screen(describe_phone_screen(info, confirm_forget));
}

void PhoneView::set_screen(PhoneScreen screen) {
    screen_ = std::move(screen);
    lines_.clear();
    badges_.clear();
    button_rects_.clear();
    code_box_ = {};

    const Typography& typography = Typography::shared();
    const float center = (kCardLeft + kCardRight) / 2.0F;
    std::vector<PhoneButtonRect> relative_buttons;   // positions relative to the card's top edge

    float y = kPaddingTop;
    icon_x_ = center;
    icon_y_ = y + kIconDiameter / 2.0F;
    y += kIconDiameter + 26.0F;

    for (const auto& line : typography.wrap(screen_.title, kTitleFont, kTitleWidth, 2)) {
        lines_.push_back({line, center, y + 36.0F, kTitleFont, kTextStrong, TextAlign::kCenter});
        y += 52.0F;
    }
    y += 12.0F;
    for (const auto& paragraph : screen_.body) {
        for (const auto& line : typography.wrap(paragraph, kBodyFont, kBodyWidth, 6)) {
            lines_.push_back({line, center, y + 24.0F, kBodyFont, kTextUnreadBody, TextAlign::kCenter});
            y += kBodyLine;
        }
        y += 14.0F;
    }

    if (!screen_.steps.empty()) {
        y += 4.0F;
        const float step_left = center - kStepBlockWidth / 2.0F;
        int number = 1;
        for (const auto& step : screen_.steps) {
            const auto wrapped = typography.wrap(step, kBodyFont, kStepBlockWidth - 56.0F, 3);
            badges_.push_back({step_left + 19.0F, y + 17.0F, number++});
            float line_y = y + 24.0F;
            for (const auto& line : wrapped) {
                lines_.push_back({line, step_left + 56.0F, line_y, kBodyFont, kTextUnreadBody, TextAlign::kLeft});
                line_y += kBodyLine;
            }
            y += std::max(38.0F, static_cast<float>(wrapped.size()) * kBodyLine) + 14.0F;
        }
    }

    if (!screen_.code.empty()) {
        y += 10.0F;
        const float code_width = typography.measure(screen_.code, kCodeFont);
        const float box_width = code_width + 120.0F;
        code_box_ = {static_cast<int>(std::lround(center - box_width / 2.0F)), static_cast<int>(std::lround(y)),
                     static_cast<int>(std::lround(center + box_width / 2.0F)), static_cast<int>(std::lround(y + 132.0F))};
        lines_.push_back({screen_.code, center, y + 100.0F, kCodeFont, kTextStrong, TextAlign::kCenter});
        y += 132.0F + 14.0F;
    }

    if (!screen_.footer.empty()) {
        y += 8.0F;
        lines_.push_back({screen_.footer, center, y + 20.0F, kFooterFont, kTextMuted, TextAlign::kCenter});
        y += 34.0F;
    }

    if (!screen_.buttons.empty()) {
        y += 18.0F;
        std::vector<std::vector<std::size_t>> rows(1);
        std::vector<float> widths;
        float row_width = 0.0F;
        for (std::size_t index = 0; index < screen_.buttons.size(); ++index) {
            const float width = std::max(kButtonMinimumWidth,
                                         typography.measure(screen_.buttons[index].label, kButtonFont) + 80.0F);
            widths.push_back(width);
            const float needed = rows.back().empty() ? width : row_width + kButtonGap + width;
            if (!rows.back().empty() && needed > kButtonMaximumRow) {
                rows.emplace_back();
                row_width = width;
            } else {
                row_width = needed;
            }
            rows.back().push_back(index);
        }
        relative_buttons.resize(screen_.buttons.size());
        for (const auto& row : rows) {
            float total = 0.0F;
            for (const std::size_t index : row) total += widths[index];
            total += kButtonGap * static_cast<float>(row.size() - 1U);
            float x = center - total / 2.0F;
            for (const std::size_t index : row) {
                relative_buttons[index] = {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y)),
                                           static_cast<int>(std::lround(x + widths[index])),
                                           static_cast<int>(std::lround(y + kButtonHeight))};
                x += widths[index] + kButtonGap;
            }
            y += kButtonHeight + kButtonGap;
        }
        y -= kButtonGap;
    }
    y += kPaddingBottom;

    // Hang the card from the top and move everything into place.
    const int shift = static_cast<int>(std::lround(kCardTop));
    card_ = {static_cast<int>(kCardLeft), shift, static_cast<int>(kCardRight), shift + static_cast<int>(std::lround(y))};
    content_height_ = std::max(static_cast<int>(kHistoryViewHeight) + kReachRoom, card_.bottom + kReachRoom);
    icon_y_ += static_cast<float>(shift);
    for (auto& line : lines_) line.baseline += static_cast<float>(shift);
    for (auto& badge : badges_) badge.y += static_cast<float>(shift);
    if (code_box_.right > code_box_.left) {
        code_box_.top += shift;
        code_box_.bottom += shift;
    }
    for (std::size_t index = 0; index < relative_buttons.size(); ++index) {
        PhoneButtonRect rect = relative_buttons[index];
        rect.top += shift;
        rect.bottom += shift;
        button_rects_.push_back(rect);
        const auto& spec = screen_.buttons[index];
        Color label_color = spec.style == ButtonStyle::kPrimary  ? kWhite
                            : spec.style == ButtonStyle::kDanger ? Color{255, 168, 178, 255}
                                                                 : Color{214, 221, 238, 255};
        lines_.push_back({spec.label, static_cast<float>(rect.left + rect.right) / 2.0F,
                          static_cast<float>(rect.top + rect.bottom) / 2.0F + typography.cap_height(kButtonFont) / 2.0F,
                          kButtonFont, label_color, TextAlign::kCenter});
    }

    Fingerprint hash;
    hash.add(static_cast<std::int64_t>(content_height_));
    hash.add(screen_.title);
    for (const auto& paragraph : screen_.body) hash.add(paragraph);
    hash.add("|");
    for (const auto& step : screen_.steps) hash.add(step);
    hash.add(screen_.code);
    hash.add(screen_.footer);
    hash.add(static_cast<std::int64_t>(screen_.tone) << 8 | static_cast<std::int64_t>(screen_.icon));
    for (const auto& spec : screen_.buttons) {
        hash.add(static_cast<std::int64_t>(spec.id) << 8 | static_cast<std::int64_t>(spec.style));
        hash.add(spec.label);
        hash.add(spec.argument);
    }
    signature_ = hash.value();
}

int PhoneView::maximum_scroll_offset() const noexcept {
    return std::max(0, content_height_ - static_cast<int>(kHistoryViewHeight));
}

std::optional<PhoneHit> PhoneView::hit_test(int x, int y, int scroll_offset) const {
    const auto px = static_cast<float>(x);
    const auto py = static_cast<float>(y + std::clamp(scroll_offset, 0, maximum_scroll_offset()));
    for (std::size_t index = 0; index < button_rects_.size(); ++index) {
        const PhoneButtonRect& rect = button_rects_[index];
        if (px >= static_cast<float>(rect.left) - kButtonHitPadding &&
            px < static_cast<float>(rect.right) + kButtonHitPadding &&
            py >= static_cast<float>(rect.top) - kButtonHitPadding &&
            py < static_cast<float>(rect.bottom) + kButtonHitPadding) {
            return PhoneHit{screen_.buttons[index].id, screen_.buttons[index].argument};
        }
    }
    return std::nullopt;
}

std::vector<std::uint8_t> PhoneView::render() const {
    Canvas canvas(kHistoryViewWidth, static_cast<std::uint32_t>(content_height_), kBackgroundBottom);
    paint_background(canvas, kPanelWidth, static_cast<float>(content_height_));
    const Color accent = tone_color(screen_.tone);

    const auto left = static_cast<float>(card_.left);
    const auto top = static_cast<float>(card_.top);
    const auto right = static_cast<float>(card_.right);
    const auto bottom = static_cast<float>(card_.bottom);
    canvas.draw_shadow(left, top, right, bottom, kCardRadius, 40.0F, 14.0F, {0, 0, 0, 130});
    canvas.fill_rounded_rect(left, top, right, bottom, kCardRadius, kCardUnreadTop, kCardUnreadBottom);
    canvas.stroke_rounded_rect(left, top, right, bottom, kCardRadius, 1.5F, mix(kBorderUnread, accent, 0.18F));

    // Icon badge in the tone's colour.
    const float radius = kIconDiameter / 2.0F;
    canvas.draw_shadow(icon_x_ - radius, icon_y_ - radius, icon_x_ + radius, icon_y_ + radius, radius, 30.0F, 8.0F,
                       faded(accent, 0.30F));
    canvas.fill_circle(icon_x_, icon_y_, radius, mix(kCardReadBottom, accent, 0.20F));
    canvas.stroke_rounded_rect(icon_x_ - radius, icon_y_ - radius, icon_x_ + radius, icon_y_ + radius, radius, 2.0F,
                               faded(accent, 0.55F));
    switch (screen_.icon) {
    case PhoneIcon::kPhone: draw_phone(canvas, icon_x_, icon_y_, accent); break;
    case PhoneIcon::kBluetooth: draw_bluetooth(canvas, icon_x_, icon_y_, accent); break;
    case PhoneIcon::kCheck: draw_check(canvas, icon_x_, icon_y_, accent); break;
    case PhoneIcon::kWarning: draw_warning(canvas, icon_x_, icon_y_, accent); break;
    case PhoneIcon::kGear: draw_gear(canvas, icon_x_, icon_y_, 64.0F, accent); break;
    }

    if (code_box_.right > code_box_.left) {
        canvas.fill_rounded_rect(static_cast<float>(code_box_.left), static_cast<float>(code_box_.top),
                                 static_cast<float>(code_box_.right), static_cast<float>(code_box_.bottom), 30.0F,
                                 {15, 19, 31, 255}, {12, 15, 25, 255});
        canvas.stroke_rounded_rect(static_cast<float>(code_box_.left), static_cast<float>(code_box_.top),
                                   static_cast<float>(code_box_.right), static_cast<float>(code_box_.bottom), 30.0F,
                                   2.0F, faded(accent, 0.7F));
    }

    for (const auto& badge : badges_) {
        canvas.fill_circle(badge.x, badge.y, 19.0F, faded(accent, 0.22F));
        canvas.stroke_rounded_rect(badge.x - 19.0F, badge.y - 19.0F, badge.x + 19.0F, badge.y + 19.0F, 19.0F, 1.5F,
                                   faded(accent, 0.6F));
        canvas.draw_text(badge.x, badge.y + Typography::shared().cap_height(kStepNumberFont) / 2.0F,
                         std::to_string(badge.number), kStepNumberFont, accent, TextAlign::kCenter);
    }

    for (std::size_t index = 0; index < button_rects_.size(); ++index) {
        const PhoneButtonRect& rect = button_rects_[index];
        const auto l = static_cast<float>(rect.left);
        const auto t = static_cast<float>(rect.top);
        const auto r = static_cast<float>(rect.right);
        const auto b = static_cast<float>(rect.bottom);
        const float pill = (b - t) / 2.0F;
        switch (screen_.buttons[index].style) {
        case ButtonStyle::kPrimary:
            canvas.draw_shadow(l, t, r, b, pill, 22.0F, 8.0F, faded(kPrimaryBottom, 0.45F));
            canvas.fill_rounded_rect(l, t, r, b, pill, kPrimaryTop, kPrimaryBottom);
            break;
        case ButtonStyle::kDanger:
            canvas.fill_rounded_rect(l, t, r, b, pill, {74, 30, 40, 255});
            canvas.stroke_rounded_rect(l, t, r, b, pill, 1.5F, {130, 48, 62, 255});
            break;
        case ButtonStyle::kSecondary:
            canvas.fill_rounded_rect(l, t, r, b, pill, kButtonFill);
            canvas.stroke_rounded_rect(l, t, r, b, pill, 1.5F, kButtonBorder);
            break;
        }
    }

    for (const auto& line : lines_) {
        canvas.draw_text(line.x, line.baseline, line.text, line.style, line.color, line.align);
    }
    return canvas.take_pixels();
}

}  // namespace frame_notify::ui
