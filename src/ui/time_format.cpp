#include "ui/time_format.h"

#include <cstdio>
#include <ctime>

namespace frame_notify::ui {
namespace {

constexpr std::int64_t kSecondsPerDay = 86400;
constexpr const char* kWeekdays[] = {"Sunday",   "Monday", "Tuesday", "Wednesday",
                                     "Thursday", "Friday", "Saturday"};
constexpr const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};

std::int64_t floor_divide(std::int64_t value, std::int64_t divisor) noexcept {
    std::int64_t quotient = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) --quotient;
    return quotient;
}

}  // namespace

std::int64_t days_from_civil(int year, int month, int day) noexcept {
    // Howard Hinnant's civil-date algorithm.
    std::int64_t y = year;
    y -= month <= 2 ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const auto year_of_era = static_cast<unsigned>(y - era * 400);
    const auto day_of_year =
        static_cast<unsigned>((153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1);
    const unsigned day_of_era =
        year_of_era * 365U + year_of_era / 4U - year_of_era / 100U + day_of_year;
    return era * 146097 + static_cast<std::int64_t>(day_of_era) - 719468;
}

CivilTime civil_from_epoch(std::int64_t epoch_seconds, int utc_offset_seconds) noexcept {
    const std::int64_t local = epoch_seconds + utc_offset_seconds;
    const std::int64_t days = floor_divide(local, kSecondsPerDay);
    const auto seconds_of_day = static_cast<int>(local - days * kSecondsPerDay);

    const std::int64_t shifted = days + 719468;
    const std::int64_t era = (shifted >= 0 ? shifted : shifted - 146096) / 146097;
    const auto day_of_era = static_cast<unsigned>(shifted - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460U + day_of_era / 36524U - day_of_era / 146096U) / 365U;
    const unsigned day_of_year = day_of_era - (365U * year_of_era + year_of_era / 4U - year_of_era / 100U);
    const unsigned month_index = (5U * day_of_year + 2U) / 153U;

    CivilTime civil;
    civil.day = static_cast<int>(day_of_year - (153U * month_index + 2U) / 5U + 1U);
    civil.month = static_cast<int>(month_index < 10U ? month_index + 3U : month_index - 9U);
    civil.year = static_cast<int>(static_cast<std::int64_t>(year_of_era) + era * 400 +
                                  (civil.month <= 2 ? 1 : 0));
    civil.hour = seconds_of_day / 3600;
    civil.minute = seconds_of_day % 3600 / 60;
    civil.second = seconds_of_day % 60;
    civil.weekday = static_cast<int>(((days % 7) + 11) % 7);  // 1970-01-01 was a Thursday
    return civil;
}

std::int64_t local_day_number(std::int64_t epoch_seconds, int utc_offset_seconds) noexcept {
    return floor_divide(epoch_seconds + utc_offset_seconds, kSecondsPerDay);
}

int local_utc_offset_seconds(std::int64_t epoch_seconds) {
    const auto moment = static_cast<std::time_t>(epoch_seconds);
    std::tm local{};
#ifdef _WIN32
    if (localtime_s(&local, &moment) != 0) return 0;
#else
    if (localtime_r(&moment, &local) == nullptr) return 0;
#endif
    const std::int64_t as_if_utc =
        days_from_civil(local.tm_year + 1900, local.tm_mon + 1, local.tm_mday) * kSecondsPerDay +
        local.tm_hour * 3600 + local.tm_min * 60 + local.tm_sec;
    return static_cast<int>(as_if_utc - epoch_seconds);
}

std::optional<std::int64_t> parse_iso8601(std::string_view text, int default_offset_seconds) {
    std::size_t position = 0;
    const auto digits = [&](int count) -> std::optional<int> {
        if (position + static_cast<std::size_t>(count) > text.size()) return std::nullopt;
        int value = 0;
        for (int index = 0; index < count; ++index) {
            const char character = text[position + static_cast<std::size_t>(index)];
            if (character < '0' || character > '9') return std::nullopt;
            value = value * 10 + (character - '0');
        }
        position += static_cast<std::size_t>(count);
        return value;
    };
    const auto expect = [&](char expected) {
        if (position < text.size() && text[position] == expected) {
            ++position;
            return true;
        }
        return false;
    };

    const auto year = digits(4);
    if (!year || !expect('-')) return std::nullopt;
    const auto month = digits(2);
    if (!month || !expect('-')) return std::nullopt;
    const auto day = digits(2);
    if (!day) return std::nullopt;
    if (!expect('T') && !expect('t') && !expect(' ')) return std::nullopt;
    const auto hour = digits(2);
    if (!hour || !expect(':')) return std::nullopt;
    const auto minute = digits(2);
    if (!minute) return std::nullopt;
    int second = 0;
    if (expect(':')) {
        const auto seconds = digits(2);
        if (!seconds) return std::nullopt;
        second = *seconds;
        if (expect('.') || expect(',')) {
            std::size_t fraction_digits = 0;
            while (position < text.size() && text[position] >= '0' && text[position] <= '9') {
                ++position;
                ++fraction_digits;
            }
            if (fraction_digits == 0) return std::nullopt;
        }
    }

    int offset = default_offset_seconds;
    if (expect('Z') || expect('z')) {
        offset = 0;
    } else if (position < text.size() && (text[position] == '+' || text[position] == '-')) {
        const int sign = text[position] == '-' ? -1 : 1;
        ++position;
        const auto offset_hours = digits(2);
        if (!offset_hours) return std::nullopt;
        expect(':');
        int offset_minutes = 0;
        if (position < text.size()) {
            const auto minutes = digits(2);
            if (!minutes) return std::nullopt;
            offset_minutes = *minutes;
        }
        if (*offset_hours > 23 || offset_minutes > 59) return std::nullopt;
        offset = sign * (*offset_hours * 3600 + offset_minutes * 60);
    }
    if (position != text.size()) return std::nullopt;

    const bool leap = (*year % 4 == 0 && *year % 100 != 0) || *year % 400 == 0;
    constexpr int kDaysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (*year < 1970 || *month < 1 || *month > 12 || *day < 1 ||
        *day > kDaysInMonth[*month - 1] + (*month == 2 && leap ? 1 : 0) || *hour > 23 ||
        *minute > 59 || second > 59) {
        return std::nullopt;
    }
    return days_from_civil(*year, *month, *day) * kSecondsPerDay + *hour * 3600 + *minute * 60 +
           second - offset;
}

std::string describe_local_time(std::int64_t epoch_seconds) {
    const int offset = local_utc_offset_seconds(epoch_seconds);
    const CivilTime civil = civil_from_epoch(epoch_seconds, offset);
    const int magnitude = offset < 0 ? -offset : offset;
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d (UTC%c%02d:%02d)", civil.year,
                  civil.month, civil.day, civil.hour, civil.minute, civil.second,
                  offset < 0 ? '-' : '+', magnitude / 3600, magnitude % 3600 / 60);
    return buffer;
}

std::string notification_time_label(std::int64_t now, std::int64_t then, int utc_offset_seconds) {
    const std::int64_t age = now - then;
    if (age < 60) return "Just now";
    if (age < 3600) return std::to_string(age / 60) + " min ago";
    const CivilTime civil = civil_from_epoch(then, utc_offset_seconds);
    char buffer[16];
    std::snprintf(buffer, sizeof(buffer), "%02d:%02d", civil.hour, civil.minute);
    return buffer;
}

std::string day_label(std::int64_t now, std::int64_t then, int utc_offset_seconds) {
    const std::int64_t difference =
        local_day_number(now, utc_offset_seconds) - local_day_number(then, utc_offset_seconds);
    if (difference <= 0) return "Today";
    if (difference == 1) return "Yesterday";
    const CivilTime civil = civil_from_epoch(then, utc_offset_seconds);
    if (difference < 7) return kWeekdays[civil.weekday];
    std::string label = std::to_string(civil.day) + " " + kMonths[civil.month - 1];
    if (civil.year != civil_from_epoch(now, utc_offset_seconds).year) {
        label += " " + std::to_string(civil.year);
    }
    return label;
}

}  // namespace frame_notify::ui
