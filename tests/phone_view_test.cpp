#include "ui/history_view.h"
#include "ui/phone_info.h"
#include "ui/phone_view.h"
#include "ui/typography.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using namespace frame_notify::ui;

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "phone_view_test.cpp:" << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(condition) expect((condition), #condition, __LINE__)

PhoneInfo info(std::string state, std::map<std::string, std::string> fields = {}, int seconds = 0) {
    PhoneInfo value;
    value.state = std::move(state);
    value.fields = std::move(fields);
    value.seconds_in_state = seconds;
    return value;
}

std::uint64_t checksum(const std::vector<std::uint8_t>& pixels) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const auto byte : pixels) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

bool has_button(const PhoneScreen& screen, PhoneButton id) {
    return std::any_of(screen.buttons.begin(), screen.buttons.end(),
                       [id](const PhoneScreenButton& button) { return button.id == id; });
}

std::size_t count_buttons(const PhoneScreen& screen, PhoneButton id) {
    return static_cast<std::size_t>(
        std::count_if(screen.buttons.begin(), screen.buttons.end(),
                      [id](const PhoneScreenButton& button) { return button.id == id; }));
}

bool mentions(const PhoneScreen& screen, const std::string& text) {
    if (screen.title.find(text) != std::string::npos || screen.footer.find(text) != std::string::npos ||
        screen.code.find(text) != std::string::npos) {
        return true;
    }
    for (const auto& paragraph : screen.body) {
        if (paragraph.find(text) != std::string::npos) return true;
    }
    for (const auto& step : screen.steps) {
        if (step.find(text) != std::string::npos) return true;
    }
    return false;
}

const std::vector<PhoneInfo>& every_state() {
    static const std::vector<PhoneInfo> states = {
        info("starting"),
        info("no_bluetooth", {{"reason", "no_bluez"}}),
        info("no_bluetooth", {{"reason", "no_adapter"}}),
        info("no_bluetooth", {{"reason", "ambiguous"}, {"detail", "hci0, hci1"}}),
        info("no_bluetooth", {{"reason", "powered_off"}}),
        info("no_bluetooth", {{"reason", "no_le"}}),
        info("unpaired"),
        info("connecting", {{"phone", "iPhone"}}),
        info("connecting", {{"phone", "iPhone"}, {"detail", "no_ancs"}}),
        info("connected", {{"phone", "iPhone"}}),
        info("needs_repair", {{"phone", "iPhone"}, {"reason", "not_allowed"}}),
        info("needs_repair", {{"phone", "iPhone"}, {"reason", "no_ancs"}}),
        info("needs_repair", {{"phone", "iPhone"}, {"reason", "not_paired"}}),
        info("pair_conflict", {{"count", "2"}, {"name1", "iPhone"}, {"address1", "AA:BB:CC:DD:EE:01"},
                               {"name2", "Pixel"}, {"address2", "AA:BB:CC:DD:EE:02"}}),
        info("pair_open", {{"seconds", "300"}, {"name", "Frame"}}),
        info("pair_confirm", {{"phone", "iPhone"}, {"code", "628640"}, {"seconds", "40"}}),
        info("pair_confirm", {{"phone", "iPhone"}, {"seconds", "40"}}),
        info("pair_verify", {{"phone", "iPhone"}}),
        info("pair_done", {{"phone", "iPhone"}, {"ancs", "yes"}}),
        info("pair_done", {{"phone", "iPhone"}, {"ancs", "no"}}),
        info("pair_failed", {{"reason", "timeout"}}),
        info("pair_failed", {{"reason", "classic_only"}, {"phone", "iPhone"}}),
        info("pair_failed", {{"reason", "advertising"}, {"detail", "Not supported"}}),
        info("pair_failed", {{"reason", "agent"}}),
        info("pair_failed", {{"reason", "error"}, {"detail", "boom"}}),
        info("helper_unavailable", {{"message", "No module named 'dbus'"}}),
        info("helper_restarting"),
        info("something_new"),
    };
    return states;
}

}  // namespace

int main() {
    EXPECT(Typography::shared().has_fonts());
    const int panel_width = static_cast<int>(kHistoryViewWidth);
    const int panel_height = static_cast<int>(kHistoryViewHeight);

    // ---- Countdown formatting ----
    EXPECT(format_countdown(272) == "4:32");
    EXPECT(format_countdown(300) == "5:00");
    EXPECT(format_countdown(65) == "1:05");
    EXPECT(format_countdown(9) == "0:09");
    EXPECT(format_countdown(0) == "0:00");
    EXPECT(format_countdown(-12) == "0:00");

    // ---- The states the helper reports, and what the panel makes of them ----
    EXPECT(phone_state_is_pairing("pair_open") && phone_state_is_pairing("pair_failed"));
    EXPECT(!phone_state_is_pairing("connected") && !phone_state_is_pairing("unpaired") &&
           !phone_state_is_pairing("helper_unavailable"));
    EXPECT(phone_state_needs_attention("pair_conflict") && phone_state_needs_attention("pair_confirm"));
    EXPECT(phone_state_needs_attention("pair_done") && phone_state_needs_attention("pair_failed"));
    EXPECT(!phone_state_needs_attention("pair_open") && !phone_state_needs_attention("pair_verify"));
    EXPECT(!phone_state_needs_attention("connected") && !phone_state_needs_attention("needs_repair"));
    EXPECT(phone_state_can_forget("connected") && phone_state_can_forget("connecting") &&
           phone_state_can_forget("needs_repair"));
    EXPECT(!phone_state_can_forget("unpaired") && !phone_state_can_forget("pair_open") &&
           !phone_state_can_forget("pair_done") && !phone_state_can_forget("helper_unavailable"));

    EXPECT(phone_chip(info("connected", {{"phone", "Pixel"}})).label == "Pixel connected");
    EXPECT(phone_chip(info("connected")).label == "iPhone connected");
    EXPECT(phone_chip(info("connected")).tone == Tone::kGood);
    EXPECT(phone_chip(info("unpaired")).label == "Pair an iPhone" && phone_chip(info("unpaired")).tone == Tone::kActive);
    EXPECT(phone_chip(info("needs_repair")).tone == Tone::kWarning);
    EXPECT(phone_chip(info("no_bluetooth", {{"reason", "powered_off"}})).label == "Bluetooth is off");
    EXPECT(phone_chip(info("no_bluetooth", {{"reason", "no_adapter"}})).label == "Bluetooth unavailable");
    EXPECT(phone_chip(info("no_bluetooth")).tone == Tone::kBad);
    EXPECT(phone_chip(info("pair_failed")).tone == Tone::kBad && phone_chip(info("pair_done")).tone == Tone::kGood);
    EXPECT(phone_chip(info("pair_open")).label == phone_chip(info("pair_verify")).label);
    EXPECT(phone_chip(info("helper_unavailable")).tone == Tone::kBad);
    EXPECT(phone_chip(info("helper_restarting")).tone == Tone::kWarning);
    EXPECT(phone_chip(info("never_heard_of_it")).label == "never_heard_of_it");
    EXPECT(info("x", {{"a", "b"}}).field("a") == "b" && info("x").field("a").empty());

    // ---- Every screen says something, offers a way out, and is laid out on the panel ----
    for (const PhoneInfo& state : every_state()) {
        const std::string label = state.state + "/" + state.field("reason") + state.field("ancs");
        const PhoneScreen screen = describe_phone_screen(state, false);
        if (screen.title.empty() || screen.buttons.size() > 4U) {
            std::cerr << "screen for " << label << " has title '" << screen.title << "' and "
                      << screen.buttons.size() << " buttons\n";
            ++failures;
        }
        if (state.state != "pair_verify" && screen.buttons.empty()) {
            std::cerr << "screen for " << label << " has no button at all\n";
            ++failures;
        }

        PhoneView view;
        view.set(state, false);
        const auto pixels = view.render();
        const int image_height = view.content_height();
        EXPECT(image_height >= panel_height + 200 && view.maximum_scroll_offset() == image_height - panel_height);
        EXPECT(pixels.size() == static_cast<std::size_t>(panel_width) * static_cast<std::size_t>(image_height) * 4U);
        EXPECT(view.button_rects().size() == screen.buttons.size());
        EXPECT(view.signature() != 0U);
        for (std::size_t index = 0; index < view.button_rects().size(); ++index) {
            const PhoneButtonRect& rect = view.button_rects()[index];
            if (rect.left < 0 || rect.top < 0 || rect.right > panel_width || rect.bottom > image_height ||
                rect.right - rect.left < 150 || rect.bottom - rect.top < 40) {
                std::cerr << "button " << index << " of " << label << " is off the panel or tiny: " << rect.left
                          << ',' << rect.top << ' ' << rect.right << ',' << rect.bottom << '\n';
                ++failures;
            }
            // The centre of every button reaches exactly that button.
            const auto hit = view.hit_test((rect.left + rect.right) / 2, (rect.top + rect.bottom) / 2);
            if (!hit || hit->button != screen.buttons[index].id || hit->argument != screen.buttons[index].argument) {
                std::cerr << "button " << index << " of " << label << " is not reached by its own centre\n";
                ++failures;
            }
            for (std::size_t other = index + 1U; other < view.button_rects().size(); ++other) {
                const PhoneButtonRect& second = view.button_rects()[other];
                const bool apart = rect.right <= second.left || second.right <= rect.left ||
                                   rect.bottom <= second.top || second.bottom <= rect.top;
                if (!apart) {
                    std::cerr << "buttons " << index << " and " << other << " of " << label << " overlap\n";
                    ++failures;
                }
            }
        }
        for (std::size_t index = 3; index < pixels.size(); index += 4) {      // a fully opaque panel
            if (pixels[index] != 255) {
                EXPECT(false);
                break;
            }
        }
        EXPECT(!view.hit_test(2, 2) && !view.hit_test(panel_width - 2, panel_height - 2, 0));
        EXPECT(!view.hit_test(-50, -50) && !view.hit_test(5000, 5000));
    }

    // Different screens are different pictures.
    {
        std::vector<std::uint64_t> signatures;
        std::vector<std::uint64_t> pictures;
        for (const PhoneInfo& state : every_state()) {
            PhoneView view;
            view.set(state, false);
            signatures.push_back(view.signature());
            pictures.push_back(checksum(view.render()));
        }
        for (std::size_t first = 0; first < signatures.size(); ++first) {
            for (std::size_t second = first + 1U; second < signatures.size(); ++second) {
                EXPECT(signatures[first] != signatures[second]);
                EXPECT(pictures[first] != pictures[second]);
            }
        }
    }

    // ---- Particular screens ----
    {   // Unpaired: the way forward is pairing.
        const auto screen = describe_phone_screen(info("unpaired"), false);
        EXPECT(screen.buttons.front().id == PhoneButton::kStartPairing && screen.buttons.front().style == ButtonStyle::kPrimary);
        EXPECT(has_button(screen, PhoneButton::kClose) && !has_button(screen, PhoneButton::kForget));
    }
    {   // Connected: forgettable, re-pairable, closable.
        const auto screen = describe_phone_screen(info("connected", {{"phone", "Pixel"}}), false);
        EXPECT(screen.tone == Tone::kGood && mentions(screen, "Pixel"));
        EXPECT(has_button(screen, PhoneButton::kForget) && has_button(screen, PhoneButton::kStartPairing) &&
               has_button(screen, PhoneButton::kClose));
        EXPECT(describe_phone_screen(info("connected"), false).title.find("your iPhone") != std::string::npos);
    }
    {   // The forget question replaces the screen, but only where forgetting makes sense.
        const auto ask = describe_phone_screen(info("connected", {{"phone", "Pixel"}}), true);
        EXPECT(mentions(ask, "Forget Pixel?"));
        EXPECT(ask.buttons.size() == 2U && ask.buttons[0].id == PhoneButton::kForgetCancelled &&
               ask.buttons[0].style == ButtonStyle::kPrimary);
        EXPECT(ask.buttons[1].id == PhoneButton::kForgetConfirmed && ask.buttons[1].style == ButtonStyle::kDanger);
        EXPECT(!has_button(describe_phone_screen(info("unpaired"), true), PhoneButton::kForgetConfirmed));
        EXPECT(!has_button(describe_phone_screen(info("pair_confirm", {{"code", "111111"}}), true),
                           PhoneButton::kForgetConfirmed));
        EXPECT(has_button(describe_phone_screen(info("needs_repair", {{"reason", "no_ancs"}}), true),
                          PhoneButton::kForgetConfirmed));
    }
    {   // Not allowed yet: the steps are the point.
        const auto screen = describe_phone_screen(info("needs_repair", {{"reason", "not_allowed"}}), false);
        EXPECT(screen.steps.size() == 3U && mentions(screen, "Share System Notifications"));
        EXPECT(has_button(screen, PhoneButton::kRetry));
    }
    {   // A conflict lists the phones that can be removed, each with its address.
        const auto screen = describe_phone_screen(
            info("pair_conflict", {{"count", "3"}, {"name1", "iPhone"}, {"address1", "AA:01"}, {"name2", "Pixel"},
                                   {"address2", "AA:02"}, {"name3", "Old phone"}, {"address3", "AA:03"}}),
            false);
        EXPECT(count_buttons(screen, PhoneButton::kRemoveConflict) == 3U);
        EXPECT(screen.buttons[0].label == "Remove iPhone" && screen.buttons[0].argument == "AA:01");
        EXPECT(screen.buttons[2].label == "Remove Old phone" && screen.buttons[2].argument == "AA:03");
        EXPECT(screen.buttons[0].style == ButtonStyle::kDanger);
        EXPECT(screen.buttons[screen.buttons.size() - 2U].id == PhoneButton::kContinueAnyway);
        EXPECT(screen.buttons.back().id == PhoneButton::kCancel);
        // Entries without a name or an address cannot be removed, so they are not offered.
        const auto partial = describe_phone_screen(
            info("pair_conflict", {{"count", "2"}, {"name1", "iPhone"}, {"name2", ""}, {"address2", "AA:02"}}), false);
        EXPECT(count_buttons(partial, PhoneButton::kRemoveConflict) == 0U);
        EXPECT(has_button(partial, PhoneButton::kContinueAnyway) && has_button(partial, PhoneButton::kCancel));
        // A very long name is cut on a character boundary and still names its address.
        const std::string long_name = std::string(30, 'W') + "\xC3\xB8" + std::string(30, 'x');
        const auto shortened = describe_phone_screen(
            info("pair_conflict", {{"name1", long_name}, {"address1", "AA:01"}}), false);
        EXPECT(shortened.buttons[0].label.size() < long_name.size() && shortened.buttons[0].argument == "AA:01");
        EXPECT(shortened.buttons[0].label.find("\xE2\x80\xA6") != std::string::npos);
        const std::string accented = std::string(19, 'a') + "\xC3\xB8" + "zzzz";
        const auto accented_screen = describe_phone_screen(
            info("pair_conflict", {{"name1", accented}, {"address1", "AA:01"}}), false);
        EXPECT(accented_screen.buttons[0].label.rfind("\xC3\xB8\xE2\x80\xA6") != std::string::npos);   // not split
    }
    {   // Waiting for the phone shows the time that is left, counted from when the state began.
        const auto fresh = describe_phone_screen(info("pair_open", {{"seconds", "300"}}, 0), false);
        EXPECT(mentions(fresh, "5:00 left"));
        const auto later = describe_phone_screen(info("pair_open", {{"seconds", "300"}}, 28), false);
        EXPECT(mentions(later, "4:30 left"));                                 // 4:32 really, in steps of five
        const auto over = describe_phone_screen(info("pair_open", {{"seconds", "300"}}, 301), false);
        EXPECT(!mentions(over, "left") && mentions(over, "Closing"));
        const auto garbage = describe_phone_screen(info("pair_open", {{"seconds", "soon"}}, 5), false);
        EXPECT(!garbage.footer.empty());
        EXPECT(describe_phone_screen(info("pair_open", {{"seconds", "300"}}), false).buttons.size() == 1U);
        EXPECT(describe_phone_screen(info("pair_open"), false).buttons[0].id == PhoneButton::kCancel);
        EXPECT(mentions(describe_phone_screen(info("pair_open", {{"detail", "rejected"}}), false), "codes differ"));
        EXPECT(mentions(describe_phone_screen(info("pair_open", {{"detail", "expired"}}), false), "in time"));
        EXPECT(describe_phone_screen(info("pair_open"), false).steps.size() == 3U);
    }
    {   // The code to compare is shown in two groups and answered with a yes or no.
        const auto screen = describe_phone_screen(
            info("pair_confirm", {{"phone", "iPhone"}, {"code", "628640"}, {"seconds", "40"}}, 9), false);
        EXPECT(screen.code == "628 640");
        EXPECT(mentions(screen, "Answer within 30 seconds"));                  // 31 really
        EXPECT(screen.buttons.size() == 2U && screen.buttons[0].id == PhoneButton::kConfirm &&
               screen.buttons[1].id == PhoneButton::kReject);
        EXPECT(screen.buttons[0].label == "Yes, it matches");
        EXPECT(describe_phone_screen(info("pair_confirm", {{"code", "12345"}}), false).code == "12345");
        // Without a code it is a plain permission question.
        const auto plain = describe_phone_screen(info("pair_confirm", {{"phone", "iPhone"}}), false);
        EXPECT(plain.code.empty() && plain.buttons[0].label == "Allow" && mentions(plain, "iPhone wants to pair"));
        EXPECT(mentions(describe_phone_screen(info("pair_confirm", {{"seconds", "40"}}, 99), false), "within 0 seconds"));
    }
    {   // The result: fully working, or one more step.
        const auto works = describe_phone_screen(info("pair_done", {{"phone", "iPhone"}, {"ancs", "yes"}}), false);
        EXPECT(works.tone == Tone::kGood && mentions(works, "Paired with iPhone"));
        // The iPhone sends nothing until the switch is on, so the steps are part of the result.
        EXPECT(works.steps.size() == 3U && mentions(works, "Share System Notifications"));
        EXPECT(mentions(describe_phone_screen(info("connected", {{"phone", "iPhone"}}), false),
                        "Share System Notifications"));
        EXPECT(works.buttons.size() == 1U && works.buttons[0].id == PhoneButton::kDismiss);
        const auto partial = describe_phone_screen(info("pair_done", {{"ancs", "no"}}), false);
        EXPECT(partial.tone == Tone::kWarning && partial.steps.size() == 3U);
        EXPECT(partial.buttons[0].id == PhoneButton::kDismiss);
    }
    {   // Failures explain themselves and offer another go.
        for (const char* reason : {"timeout", "classic_only", "advertising", "agent", "error", "what"}) {
            const auto screen = describe_phone_screen(info("pair_failed", {{"reason", reason}}), false);
            EXPECT(screen.tone == Tone::kBad);
            EXPECT(screen.buttons.size() == 2U && screen.buttons[0].id == PhoneButton::kStartPairing);
            EXPECT(screen.buttons[1].id == PhoneButton::kDismiss);
        }
        EXPECT(mentions(describe_phone_screen(info("pair_failed", {{"reason", "classic_only"}}), false), "Forget This Device"));
        EXPECT(mentions(describe_phone_screen(info("pair_failed", {{"reason", "advertising"}, {"detail", "Busy"}}), false), "Busy"));
        EXPECT(mentions(describe_phone_screen(info("pair_failed", {{"reason", "error"}, {"detail", "kaboom"}}), false), "kaboom"));
    }
    {   // Bluetooth problems: turn it on, or try again.
        EXPECT(has_button(describe_phone_screen(info("no_bluetooth", {{"reason", "powered_off"}}), false),
                          PhoneButton::kPowerOn));
        for (const char* reason : {"no_bluez", "no_adapter", "ambiguous", "no_le"}) {
            const auto screen = describe_phone_screen(info("no_bluetooth", {{"reason", reason}}), false);
            EXPECT(has_button(screen, PhoneButton::kRetry) && !has_button(screen, PhoneButton::kPowerOn));
        }
        EXPECT(mentions(describe_phone_screen(info("no_bluetooth", {{"reason", "ambiguous"}, {"detail", "hci0, hci1"}}), false),
                        "hci0, hci1"));
        const auto helper = describe_phone_screen(info("helper_unavailable", {{"message", "No module named 'dbus'"}}), false);
        EXPECT(mentions(helper, "No module named 'dbus'") && has_button(helper, PhoneButton::kRetry));
        EXPECT(!describe_phone_screen(info("helper_unavailable"), false).body.empty());
    }

    // ---- Hit testing ----
    {
        PhoneView view;
        view.set(info("pair_confirm", {{"phone", "iPhone"}, {"code", "628640"}, {"seconds", "40"}}), false);
        const auto& rects = view.button_rects();
        EXPECT(rects.size() == 2U);
        const auto yes = view.hit_test((rects[0].left + rects[0].right) / 2, (rects[0].top + rects[0].bottom) / 2);
        const auto no = view.hit_test((rects[1].left + rects[1].right) / 2, (rects[1].top + rects[1].bottom) / 2);
        EXPECT(yes && yes->button == PhoneButton::kConfirm && no && no->button == PhoneButton::kReject);
        EXPECT(view.hit_test(rects[0].left - 4, rects[0].top + 10)->button == PhoneButton::kConfirm);   // forgiving
        EXPECT(!view.hit_test(rects[0].left - 30, rects[0].top + 10));
        EXPECT(!view.hit_test(rects[0].left + 10, rects[0].top - 30));
        EXPECT(!view.hit_test(rects[0].left + 10, rects[0].bottom + 30));
        EXPECT(!view.hit_test(640, 60));                                   // the icon and title are not buttons
    }
    {
        PhoneView view;
        view.set(info("pair_conflict", {{"name1", "iPhone"}, {"address1", "AA:01"}, {"name2", "Pixel"},
                                        {"address2", "AA:02"}}),
                 false);
        const auto& rects = view.button_rects();
        const auto second = view.hit_test((rects[1].left + rects[1].right) / 2, (rects[1].top + rects[1].bottom) / 2);
        EXPECT(second && second->button == PhoneButton::kRemoveConflict && second->argument == "AA:02");
    }

    // ---- Settings ----
    {
        SettingsInfo off;
        off.version = "1.2.3";
        const PhoneScreen screen = describe_settings_screen(off);
        EXPECT(screen.title == "Settings" && screen.icon == PhoneIcon::kGear);
        EXPECT(mentions(screen, "Start automatically: off") && mentions(screen, "Frame Notify 1.2.3"));
        EXPECT(screen.buttons.size() == 2U && screen.buttons[0].id == PhoneButton::kToggleAutostart);
        EXPECT(screen.buttons[0].label == "Turn on autostart" && screen.buttons[0].style == ButtonStyle::kPrimary);
        EXPECT(screen.buttons[1].id == PhoneButton::kClose);

        SettingsInfo on;
        on.autostart_enabled = true;
        on.autostart_method = "systemd";
        const PhoneScreen enabled = describe_settings_screen(on);
        EXPECT(mentions(enabled, "Start automatically: on") && enabled.tone == Tone::kGood);
        EXPECT(enabled.buttons.size() == 2U && enabled.buttons[0].id == PhoneButton::kClose &&
               enabled.buttons[0].style == ButtonStyle::kPrimary);
        EXPECT(enabled.buttons[1].id == PhoneButton::kToggleAutostart && enabled.buttons[1].label == "Turn off autostart");
        EXPECT(!mentions(enabled, "desktop autostart entry") && enabled.footer.empty());
        on.autostart_method = "desktop";
        EXPECT(mentions(describe_settings_screen(on), "desktop autostart entry"));

        SettingsInfo failed;
        failed.message = "systemd: Failed to enable: boom";
        const PhoneScreen problem = describe_settings_screen(failed);
        EXPECT(problem.tone == Tone::kWarning && mentions(problem, "Failed to enable: boom"));

        // It lays out and draws like any other card screen, and its buttons answer to presses.
        PhoneView view;
        view.set_screen(describe_settings_screen(off));
        EXPECT(view.render().size() == static_cast<std::size_t>(panel_width) * static_cast<std::size_t>(view.content_height()) * 4U);
        EXPECT(view.button_rects().size() == 2U);
        const PhoneButtonRect toggle = view.button_rects()[0];
        const auto hit = view.hit_test((toggle.left + toggle.right) / 2, (toggle.top + toggle.bottom) / 2);
        EXPECT(hit && hit->button == PhoneButton::kToggleAutostart);
        EXPECT(toggle.bottom < panel_height * 7 / 10);
        PhoneView enabled_view;
        enabled_view.set_screen(describe_settings_screen(SettingsInfo{true, "systemd", {}, {}}));
        EXPECT(view.signature() != enabled_view.signature());
        EXPECT(checksum(view.render()) != checksum(enabled_view.render()));
        PhoneView long_problem;
        SettingsInfo wordy;
        wordy.message = std::string(3000, 'w');
        long_problem.set_screen(describe_settings_screen(wordy));
        EXPECT(long_problem.render().size() ==
               static_cast<std::size_t>(panel_width) * static_cast<std::size_t>(long_problem.content_height()) * 4U);
        for (const auto& rect : long_problem.button_rects()) EXPECT(rect.bottom <= long_problem.content_height());
    }

    // ---- Reach: buttons hang high in the panel, and the screen can be dragged up ----
    {
        // The usual screens keep their buttons in the upper part of the panel.
        for (const PhoneInfo& state : {info("unpaired"), info("connected", {{"phone", "iPhone"}}),
                                       info("pair_open", {{"seconds", "300"}}),
                                       info("no_bluetooth", {{"reason", "powered_off"}}),
                                       info("pair_failed", {{"reason", "timeout"}})}) {
            PhoneView view;
            view.set(state, false);
            for (const auto& rect : view.button_rects()) {
                if (rect.bottom > panel_height * 7 / 10) {
                    std::cerr << "a button of '" << state.state << "' sits low in the panel: " << rect.bottom << '\n';
                    ++failures;
                }
            }
        }
        // Screens with instructions are taller; their buttons still stay on the first screenful.
        for (const PhoneInfo& state : {info("pair_done", {{"phone", "iPhone"}, {"ancs", "yes"}}),
                                       info("pair_done", {{"phone", "iPhone"}, {"ancs", "no"}}),
                                       info("needs_repair", {{"phone", "iPhone"}, {"reason", "not_allowed"}})}) {
            PhoneView steps;
            steps.set(state, false);
            for (const auto& rect : steps.button_rects()) EXPECT(rect.bottom < panel_height * 4 / 5);
        }
        // The code screen is the tallest common one; its answer buttons stay on the first screenful.
        PhoneView confirm;
        confirm.set(info("pair_confirm", {{"phone", "iPhone"}, {"code", "628640"}, {"seconds", "40"}}), false);
        for (const auto& rect : confirm.button_rects()) EXPECT(rect.bottom < panel_height * 3 / 4);

        // Dragging the screen up brings a button to a higher row, and hit-testing follows.
        PhoneView view;
        view.set(info("connected", {{"phone", "iPhone"}}), false);
        const PhoneButtonRect first = view.button_rects()[0];
        const int centre_x = (first.left + first.right) / 2;
        const int centre_y = (first.top + first.bottom) / 2;
        EXPECT(view.hit_test(centre_x, centre_y, 0)->button == PhoneButton::kClose);
        EXPECT(!view.hit_test(centre_x, centre_y - 150, 0));
        EXPECT(view.hit_test(centre_x, centre_y - 150, 150)->button == PhoneButton::kClose);
        EXPECT(!view.hit_test(centre_x, centre_y, 150));                      // that spot has moved away
        const int furthest = view.maximum_scroll_offset();
        EXPECT(view.hit_test(centre_x, centre_y - furthest, furthest)->button == PhoneButton::kClose);
        EXPECT(view.hit_test(centre_x, centre_y - furthest, 40000)->button == PhoneButton::kClose);   // clamped
        EXPECT(view.hit_test(centre_x, centre_y, -500)->button == PhoneButton::kClose);   // never above the top
        EXPECT(view.maximum_scroll_offset() >= 200);                           // real room to drag
        // A taller card than the panel still fits in the image, with room under it.
        PhoneView tall;
        tall.set(info("pair_conflict", {{"name1", std::string(4000, 'W')}, {"address1", "AA:01"},
                                        {"name2", std::string(4000, 'W')}, {"address2", "AA:02"},
                                        {"name3", std::string(4000, 'W')}, {"address3", "AA:03"}}),
                 false);
        EXPECT(tall.content_height() >= tall.button_rects().back().bottom + 200);
    }

    // ---- Signatures: a new second on the clock changes the picture, an unchanged screen does not ----
    {
        PhoneView a;
        PhoneView b;
        a.set(info("pair_open", {{"seconds", "300"}}, 11), false);
        b.set(info("pair_open", {{"seconds", "300"}}, 11), false);
        EXPECT(a.signature() == b.signature() && checksum(a.render()) == checksum(b.render()));
        b.set(info("pair_open", {{"seconds", "300"}}, 15), false);           // 285 left either way: same step
        EXPECT(a.signature() == b.signature());
        b.set(info("pair_open", {{"seconds", "300"}}, 16), false);           // 284 left: the next step
        EXPECT(a.signature() != b.signature() && checksum(a.render()) != checksum(b.render()));
        b.set(info("pair_open", {{"seconds", "300"}}, 11), true);            // forget is ignored here
        EXPECT(a.signature() == b.signature());
        PhoneView c;
        PhoneView d;
        c.set(info("connected", {{"phone", "iPhone"}}), false);
        d.set(info("connected", {{"phone", "iPhone"}}), true);
        EXPECT(c.signature() != d.signature() && checksum(c.render()) != checksum(d.render()));
        EXPECT(c.button_rects().size() == 3U && d.button_rects().size() == 2U);
        for (int round = 0; round < 10; ++round) {                            // deterministic, whatever the heap holds
            std::vector<std::vector<char>> noise;
            for (int fill = 0; fill < round * 4; ++fill) noise.emplace_back(static_cast<std::size_t>(30 + fill * 9), 'q');
            PhoneView again;
            again.set(info("connected", {{"phone", "iPhone"}}), false);
            EXPECT(again.signature() == c.signature());
        }
    }

    // ---- Hostile text cannot break the layout or push buttons off the panel ----
    {
        const std::string huge(4000, 'W');
        const std::string emoji = "\xF0\x9F\x93\xB1 \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD \xE2\x80\x8D";
        std::vector<PhoneInfo> nasty = {
            info("pair_conflict", {{"name1", huge}, {"address1", "AA:01"}, {"name2", emoji}, {"address2", "AA:02"},
                                   {"name3", huge}, {"address3", "AA:03"}}),
            info("pair_failed", {{"reason", "error"}, {"detail", huge}}),
            info("pair_failed", {{"reason", "advertising"}, {"detail", huge}}),
            info("helper_unavailable", {{"message", huge}}),
            info("connected", {{"phone", huge}}),
            info("connected", {{"phone", emoji}}),
            info("pair_confirm", {{"phone", huge}, {"code", std::string(200, '7')}, {"seconds", "99999999999999"}}),
            info("pair_open", {{"seconds", "-5"}}, -40),
            info("no_bluetooth", {{"reason", "ambiguous"}, {"detail", huge}}),
            info("", {}),
        };
        for (const PhoneInfo& state : nasty) {
            for (const bool forget : {false, true}) {
                PhoneView view;
                view.set(state, forget);
                EXPECT(view.render().size() ==
                       static_cast<std::size_t>(panel_width) * static_cast<std::size_t>(view.content_height()) * 4U);
                for (const auto& rect : view.button_rects()) {
                    if (rect.bottom > view.content_height() || rect.top < 0) {
                        std::cerr << "a button of '" << state.state << "' left the panel: " << rect.top << ".." << rect.bottom << '\n';
                        ++failures;
                    }
                }
            }
        }
    }

    if (failures != 0) {
        std::cerr << failures << " expectation(s) failed\n";
        return 1;
    }
    return 0;
}
