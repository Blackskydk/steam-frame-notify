#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace frame_notify::ui {

struct CivilTime {
    int year = 1970;
    int month = 1;    // 1-12
    int day = 1;      // 1-31
    int hour = 0;
    int minute = 0;
    int second = 0;
    int weekday = 4;  // 0 = Sunday
};

// Days since 1970-01-01 for a proleptic Gregorian date.
[[nodiscard]] std::int64_t days_from_civil(int year, int month, int day) noexcept;
[[nodiscard]] CivilTime civil_from_epoch(std::int64_t epoch_seconds, int utc_offset_seconds) noexcept;
// The local calendar day number of an instant; equal numbers mean the same local day.
[[nodiscard]] std::int64_t local_day_number(std::int64_t epoch_seconds,
                                            int utc_offset_seconds) noexcept;
// Offset of the system time zone from UTC at the given instant, in seconds.
[[nodiscard]] int local_utc_offset_seconds(std::int64_t epoch_seconds);

// Parses an ISO 8601 / RFC 3339 date-time such as "2026-09-29T17:02:00+02:00", "...Z", or with a
// space instead of the T and fractional seconds. A time without a zone is read in the zone given
// by `default_offset_seconds`. Anything else, including impossible dates, gives no value.
[[nodiscard]] std::optional<std::int64_t> parse_iso8601(std::string_view text,
                                                        int default_offset_seconds);

// "2026-09-30 09:41:05 (UTC+02:00)", for logs.
[[nodiscard]] std::string describe_local_time(std::int64_t epoch_seconds);

// "Just now" and "12 min ago" for the last hour, otherwise the local clock time such as "14:02".
[[nodiscard]] std::string notification_time_label(std::int64_t now, std::int64_t then,
                                                  int utc_offset_seconds);
// "Today", "Yesterday", a weekday name for the past week, otherwise "12 Sep" (with the year when
// it is not the current one).
[[nodiscard]] std::string day_label(std::int64_t now, std::int64_t then, int utc_offset_seconds);

}  // namespace frame_notify::ui
