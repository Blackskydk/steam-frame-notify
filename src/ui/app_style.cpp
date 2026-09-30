#include "ui/app_style.h"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <iterator>

namespace frame_notify::ui {
namespace {

struct KnownApp {
    const char* id;    // lower-case bundle identifier
    const char* name;  // display name
    Color accent;
};

constexpr KnownApp kKnownApps[] = {
    {"com.apple.mobilesms", "Messages", {52, 199, 89, 255}},
    {"com.apple.mobilephone", "Phone", {52, 199, 89, 255}},
    {"com.apple.facetime", "FaceTime", {52, 199, 89, 255}},
    {"com.apple.mobilemail", "Mail", {61, 139, 255, 255}},
    {"com.apple.mobilecal", "Calendar", {255, 90, 82, 255}},
    {"com.apple.reminders", "Reminders", {255, 159, 10, 255}},
    {"com.apple.mobilenotes", "Notes", {245, 179, 1, 255}},
    {"com.apple.passbook", "Wallet", {242, 169, 59, 255}},
    {"com.apple.appstore", "App Store", {47, 140, 255, 255}},
    {"com.apple.health", "Health", {255, 55, 95, 255}},
    {"com.apple.findmy", "Find My", {52, 199, 89, 255}},
    {"com.apple.weather", "Weather", {79, 163, 247, 255}},
    {"com.apple.maps", "Maps", {52, 199, 89, 255}},
    {"com.apple.music", "Music", {250, 45, 72, 255}},
    {"com.apple.podcasts", "Podcasts", {177, 94, 255, 255}},
    {"com.apple.tv", "TV", {110, 118, 136, 255}},
    {"com.apple.news", "News", {255, 55, 95, 255}},
    {"com.apple.shortcuts", "Shortcuts", {123, 97, 255, 255}},
    {"com.apple.home", "Home", {255, 159, 10, 255}},
    {"com.apple.screentime", "Screen Time", {123, 97, 255, 255}},
    {"com.apple.mobileslideshow", "Photos", {255, 122, 89, 255}},
    {"com.apple.mobiletimer", "Clock", {255, 159, 10, 255}},
    {"net.whatsapp.whatsapp", "WhatsApp", {37, 211, 102, 255}},
    {"ph.telegra.telegraph", "Telegram", {42, 171, 238, 255}},
    {"com.hammerandchisel.discord", "Discord", {88, 101, 242, 255}},
    {"com.tinyspeck.chatlyio", "Slack", {177, 79, 184, 255}},
    {"org.whispersystems.signal", "Signal", {58, 118, 240, 255}},
    {"com.facebook.messenger", "Messenger", {10, 132, 255, 255}},
    {"com.facebook.facebook", "Facebook", {24, 119, 242, 255}},
    {"com.burbn.instagram", "Instagram", {225, 48, 108, 255}},
    {"com.atebits.tweetie2", "X", {165, 174, 192, 255}},
    {"com.reddit.reddit", "Reddit", {255, 69, 0, 255}},
    {"com.google.gmail", "Gmail", {234, 67, 53, 255}},
    {"com.google.chrome.ios", "Chrome", {66, 133, 244, 255}},
    {"com.spotify.client", "Spotify", {29, 185, 84, 255}},
    {"com.netflix.netflix", "Netflix", {229, 9, 20, 255}},
    {"com.zhiliaoapp.musically", "TikTok", {254, 44, 85, 255}},
    {"com.linkedin.linkedin", "LinkedIn", {10, 102, 194, 255}},
    {"com.microsoft.office.outlook", "Outlook", {10, 120, 212, 255}},
    {"com.microsoft.skype.teams", "Teams", {98, 100, 167, 255}},
    {"io.heckel.ntfy", "ntfy", {47, 184, 154, 255}},
    {"com.valvesoftware.steammobile", "Steam", {102, 192, 244, 255}},
};

constexpr Color kFallbackPalette[] = {
    {91, 140, 255, 255},  {155, 123, 255, 255}, {51, 195, 165, 255}, {255, 180, 84, 255},
    {255, 107, 139, 255}, {76, 201, 240, 255},  {139, 212, 80, 255}, {242, 123, 208, 255},
};

std::string lowercase(std::string_view text) {
    std::string result(text);
    for (char& character : result) {
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    }
    return result;
}

std::uint32_t hash(std::string_view text) {
    std::uint32_t value = 2166136261U;
    for (const char character : text) {
        value ^= static_cast<unsigned char>(character);
        value *= 16777619U;
    }
    return value;
}

const KnownApp* find_known(std::string_view key) {
    const std::string lowered = lowercase(key);
    for (const KnownApp& app : kKnownApps) {
        if (lowered == app.id || lowered == lowercase(app.name)) return &app;
    }
    return nullptr;
}

// "io.heckel.ntfy" -> "Ntfy". Only used for apps that are not in the table above.
std::string name_from_bundle_id(std::string_view id) {
    std::string_view name = id;
    while (true) {
        const std::size_t dot = name.rfind('.');
        const std::string_view last = dot == std::string_view::npos ? name : name.substr(dot + 1U);
        const std::string lowered = lowercase(last);
        const bool generic = lowered == "app" || lowered == "ios" || lowered == "mobile" ||
                             lowered == "client" || lowered == "iphone" || lowered == "ipad" ||
                             last.empty();
        if (!generic || dot == std::string_view::npos) {
            name = last;
            break;
        }
        name = name.substr(0, dot);
    }
    std::string result(name);
    if (!result.empty()) {
        result[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(result[0])));
    }
    return result;
}

// Upper-case a UTF-8 letter: ASCII plus the Latin-1 and Latin Extended-A letters used in Nordic languages.
std::string first_letter_uppercase(std::string_view text) {
    std::size_t offset = 0;
    while (offset < text.size()) {
        const auto lead = static_cast<unsigned char>(text[offset]);
        std::size_t length = 1;
        char32_t code_point = lead;
        if ((lead & 0xE0U) == 0xC0U && offset + 1U < text.size()) {
            length = 2;
            code_point = ((lead & 0x1FU) << 6U) | (static_cast<unsigned char>(text[offset + 1U]) & 0x3FU);
        } else if ((lead & 0xF0U) == 0xE0U && offset + 2U < text.size()) {
            length = 3;
            code_point = ((lead & 0x0FU) << 12U) |
                         ((static_cast<unsigned char>(text[offset + 1U]) & 0x3FU) << 6U) |
                         (static_cast<unsigned char>(text[offset + 2U]) & 0x3FU);
        } else if ((lead & 0xF8U) == 0xF0U && offset + 3U < text.size()) {
            length = 4;
            code_point = 0x1F000U;  // emoji and other symbols are not used as monograms
        } else if (lead >= 0x80U) {
            ++offset;
            continue;
        }
        const bool ascii_letter = code_point < 0x80U && std::isalnum(static_cast<int>(code_point)) != 0;
        const bool latin_letter = code_point >= 0xC0U && code_point <= 0x24FU && code_point != 0xD7U &&
                                  code_point != 0xF7U;
        if (ascii_letter || latin_letter) {
            if (ascii_letter) code_point = static_cast<char32_t>(std::toupper(static_cast<int>(code_point)));
            else if (code_point >= 0xE0U && code_point <= 0xFEU && code_point != 0xF7U) code_point -= 0x20U;
            else if (code_point >= 0x100U && code_point <= 0x17FU && (code_point & 1U) != 0U) code_point -= 1U;
            std::string result;
            if (code_point < 0x80U) {
                result.push_back(static_cast<char>(code_point));
            } else {
                result.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
                result.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
            }
            return result;
        }
        offset += length;
    }
    return "?";
}

}  // namespace

bool looks_like_bundle_id(std::string_view text) noexcept {
    if (text.size() < 3U || text.find('.') == std::string_view::npos || text.front() == '.' ||
        text.back() == '.') {
        return false;
    }
    for (const char character : text) {
        const auto value = static_cast<unsigned char>(character);
        if (!(std::isalnum(value) != 0 || character == '.' || character == '-' || character == '_')) {
            return false;
        }
    }
    return true;
}

AppStyle resolve_app_style(std::string_view app, std::string_view app_id) {
    const KnownApp* known = nullptr;
    if (!app_id.empty()) known = find_known(app_id);
    if (known == nullptr && !app.empty()) known = find_known(app);

    AppStyle style;
    if (!app.empty() && !looks_like_bundle_id(app)) {
        style.name = std::string(app);
    } else if (known != nullptr) {
        style.name = known->name;
    } else if (!app.empty()) {
        style.name = name_from_bundle_id(app);
    } else if (!app_id.empty()) {
        style.name = name_from_bundle_id(app_id);
    } else {
        style.name = "Notification";
    }
    style.accent = known != nullptr
                       ? known->accent
                       : kFallbackPalette[hash(lowercase(style.name)) % std::size(kFallbackPalette)];
    style.monogram = first_letter_uppercase(style.name);
    return style;
}

}  // namespace frame_notify::ui
