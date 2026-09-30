#include "ui/app_style.h"
#include "ui/font.h"
#include "ui/raster.h"
#include "ui/renderer.h"
#include "ui/scroll_controller.h"
#include "ui/time_format.h"
#include "ui/typography.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <iostream>
#include <string>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "ui_test.cpp:" << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(condition) expect((condition), #condition, __LINE__)

using namespace frame_notify::ui;

double total(const Coverage& coverage) {
    double sum = 0.0;
    for (const float value : coverage.alpha) sum += static_cast<double>(value);
    return sum;
}

float at(const Coverage& coverage, int x, int y) {
    return coverage.alpha[static_cast<std::size_t>(y - coverage.top) *
                              static_cast<std::size_t>(coverage.width) +
                          static_cast<std::size_t>(x - coverage.left)];
}

Path rectangle(float left, float top, float right, float bottom) {
    Path path;
    path.move_to({left, top});
    path.line_to({right, top});
    path.line_to({right, bottom});
    path.line_to({left, bottom});
    path.close();
    return path;
}

void test_rasterizer() {
    const Coverage square = rasterize(rectangle(2, 3, 6, 7), 0, 0, 10, 10);
    EXPECT(std::fabs(total(square) - 16.0) < 1.0e-3);
    EXPECT(at(square, 2, 3) > 0.999F && at(square, 5, 6) > 0.999F);

    // A rectangle on half-pixel boundaries covers a quarter of each corner pixel.
    const Coverage shifted = rasterize(rectangle(0.5F, 0.5F, 2.5F, 2.5F), 0, 0, 10, 10);
    EXPECT(std::fabs(total(shifted) - 4.0) < 1.0e-3);
    EXPECT(std::fabs(at(shifted, 0, 0) - 0.25F) < 1.0e-3F);
    EXPECT(std::fabs(at(shifted, 1, 0) - 0.5F) < 1.0e-3F);
    EXPECT(at(shifted, 1, 1) > 0.999F);

    // Clipping removes what lies outside the buffer without distorting what is inside.
    const Coverage clipped = rasterize(rectangle(-5, -5, 5, 5), 0, 0, 10, 10);
    EXPECT(std::fabs(total(clipped) - 25.0) < 1.0e-3);
    const Coverage overhang = rasterize(rectangle(6, 2, 40, 4), 0, 0, 10, 10);
    EXPECT(std::fabs(total(overhang) - 8.0) < 1.0e-3);

    Path circle;
    circle.add_circle({20.0F, 20.0F}, 10.0F);
    const double disc = total(rasterize(circle, 0, 0, 40, 40));
    EXPECT(std::fabs(disc - 3.14159265 * 100.0) < 3.0);

    // A reversed circle inside another leaves a hole; overlapping same-way shapes count once.
    Path ring;
    ring.add_circle({20.0F, 20.0F}, 10.0F);
    ring.add_circle({20.0F, 20.0F}, 5.0F, true);
    EXPECT(std::fabs(total(rasterize(ring, 0, 0, 40, 40)) - 3.14159265 * 75.0) < 3.0);
    Path overlap = rectangle(1, 1, 5, 5);
    const Path second = rectangle(3, 3, 7, 7);
    for (const auto& contour : second.contours()) {
        overlap.move_to(contour[0]);
        for (std::size_t index = 1; index < contour.size(); ++index) overlap.line_to(contour[index]);
        overlap.close();
    }
    EXPECT(std::fabs(total(rasterize(overlap, 0, 0, 10, 10)) - 28.0) < 1.0e-3);

    EXPECT(rasterize(Path{}, 0, 0, 10, 10).empty());
}

void test_font() {
    const std::string directory = Typography::find_font_directory();
    EXPECT(!directory.empty());
    const auto font = Font::load(directory + "/Inter-Regular.ttf");
    EXPECT(font != nullptr);
    if (!font) return;

    EXPECT(font->units_per_em() > 0 && font->ascender() > 0 && font->descender() < 0);
    EXPECT(font->cap_height() > 0 && font->cap_height() < font->units_per_em());
    const auto glyph_a = font->glyph_index(U'A');
    const auto glyph_v = font->glyph_index(U'V');
    EXPECT(glyph_a != 0 && glyph_v != 0 && glyph_a != glyph_v);
    EXPECT(font->glyph_index(0x1F600) == 0);  // emoji are not in the font
    EXPECT(font->glyph_index(U'Å') != 0 && font->glyph_index(U'ø') != 0);
    EXPECT(font->advance(glyph_a) > 0);
    EXPECT(font->kerning(glyph_a, glyph_v) < 0);
    EXPECT(font->kerning(0, glyph_v) == 0);
    EXPECT(!font->outline(glyph_a).empty());
    // A-ring is a composite glyph: it must contain more contours than plain A.
    EXPECT(font->outline(font->glyph_index(U'Å')).contours().size() >
           font->outline(glyph_a).contours().size());

    EXPECT(Font::load(directory + "/does-not-exist.ttf") == nullptr);
    EXPECT(Font::from_memory(std::vector<std::uint8_t>(64, 0x42)) == nullptr);
    EXPECT(Font::from_memory({}) == nullptr);
}

void test_typography() {
    const Typography& typography = Typography::shared();
    EXPECT(typography.has_fonts());
    TextStyle regular{24.0F, FontWeight::kRegular, 0.0F};
    TextStyle bold{24.0F, FontWeight::kSemiBold, 0.0F};

    EXPECT(typography.measure("", regular) == 0.0F);
    EXPECT(typography.measure("Hello", regular) > 40.0F);
    EXPECT(typography.measure("Hello world", regular) > typography.measure("Hello", regular));
    EXPECT(typography.measure("Hello world", bold) > typography.measure("Hello world", regular));
    EXPECT(typography.measure("AV", regular) <
           typography.measure("A", regular) + typography.measure("V", regular));  // kerning
    TextStyle tracked = regular;
    tracked.tracking = 2.0F;
    EXPECT(std::fabs(typography.measure("abcd", tracked) - typography.measure("abcd", regular) - 6.0F) <
           0.01F);

    // Zero-width joiners and variation selectors take no space; an emoji run is one placeholder.
    EXPECT(typography.measure("a\xE2\x80\x8D" "b", regular) == typography.measure("ab", regular));
    const float one_emoji = typography.measure("\xF0\x9F\x98\x80", regular);
    EXPECT(one_emoji > 0.0F);
    EXPECT(typography.measure("\xF0\x9F\x98\x80\xF0\x9F\x91\x8D\xE2\x80\x8D\xF0\x9F\x8F\xBD", regular) ==
           one_emoji);
    EXPECT(!typography.layout("\xF0\x9F\x98\x80", regular, 0.0F).empty());

    const auto glyphs = typography.layout("Hello", regular, 10.25F);
    EXPECT(glyphs.size() == 5U);
    for (const auto& glyph : glyphs) {
        std::uint32_t ink = 0;
        for (const std::uint8_t value : glyph.image->alpha) ink += value;
        EXPECT(ink > 0U);
        EXPECT(glyph.image->top < 0);  // ink sits above the baseline
    }
    EXPECT(typography.cap_height(regular) > 12.0F && typography.cap_height(regular) < 24.0F);

    // Wrapping.
    const std::string sentence =
        "Are you coming home soon? I made dinner and it is getting cold so hurry up";
    const float width = 300.0F;
    const auto lines = typography.wrap(sentence, regular, width, 10);
    EXPECT(lines.size() >= 3U);
    for (const auto& line : lines) EXPECT(typography.measure(line, regular) <= width + 0.01F);
    std::string rejoined;
    for (const auto& line : lines) rejoined += (rejoined.empty() ? "" : " ") + line;
    EXPECT(rejoined == sentence);

    const auto truncated = typography.wrap(sentence, regular, width, 2);
    EXPECT(truncated.size() == 2U);
    if (truncated.size() == 2U) {
        EXPECT(truncated[1].size() >= 3U && truncated[1].substr(truncated[1].size() - 3U) ==
                                                "\xE2\x80\xA6");
        EXPECT(typography.measure(truncated[1], regular) <= width + 0.01F);
    }
    EXPECT(typography.wrap("one\ntwo\n\nthree", regular, 500.0F, 5).size() == 3U);
    EXPECT(typography.wrap("", regular, 100.0F, 3).empty());
    EXPECT(typography.wrap("text", regular, 100.0F, 0).empty());

    const auto url = typography.wrap("https://example.com/a/very/long/path/without/any/spaces/at/all/really",
                                     regular, 200.0F, 6);
    EXPECT(url.size() >= 2U);
    for (const auto& line : url) EXPECT(typography.measure(line, regular) <= 200.01F);

    const std::string short_text = typography.ellipsize("Short", regular, 400.0F);
    EXPECT(short_text == "Short");
    const std::string cut = typography.ellipsize(sentence, regular, 250.0F);
    EXPECT(cut != sentence && typography.measure(cut, regular) <= 250.01F);
    EXPECT(cut.size() >= 3U && cut.substr(cut.size() - 3U) == "\xE2\x80\xA6");
    // Multi-byte characters are never split in the middle.
    const std::string accented =
        typography.ellipsize("Z\xC3\xBC" "rich \xC3\x85" "land na\xC3\xAF" "ve caf\xC3\xA9", regular, 90.0F);
    EXPECT(accented.size() >= 3U && accented.substr(accented.size() - 3U) == "\xE2\x80\xA6");
    for (std::size_t index = 0; index < accented.size(); ++index) {
        // Every lead byte must be followed by its continuation bytes: no split characters.
        const auto byte = static_cast<unsigned char>(accented[index]);
        if (byte >= 0xC0U) {
            const std::size_t needed = byte >= 0xF0U ? 3U : byte >= 0xE0U ? 2U : 1U;
            EXPECT(index + needed < accented.size());
        }
    }

    // Without font files the built-in bitmap font still measures, wraps and draws.
    const Typography fallback("/definitely/not/a/font/directory");
    EXPECT(!fallback.has_fonts());
    EXPECT(fallback.measure("abc", regular) > 0.0F);
    EXPECT(!fallback.layout("abc", regular, 0.0F).empty());
    EXPECT(!fallback.wrap(sentence, regular, 300.0F, 4).empty());
}

void test_app_style() {
    const AppStyle messages = resolve_app_style("com.apple.MobileSMS");
    EXPECT(messages.name == "Messages" && messages.monogram == "M");
    EXPECT(messages.accent.red == 52 && messages.accent.green == 199 && messages.accent.blue == 89);

    // A readable name from the sender wins, the identifier still supplies the colour.
    const AppStyle localized = resolve_app_style("Nachrichten", "com.apple.MobileSMS");
    EXPECT(localized.name == "Nachrichten" && localized.monogram == "N");
    EXPECT(localized.accent.green == messages.accent.green && localized.accent.red == messages.accent.red);

    EXPECT(resolve_app_style("io.heckel.ntfy").name == "ntfy");
    EXPECT(resolve_app_style("Messages").accent.green == messages.accent.green);  // name lookup
    EXPECT(resolve_app_style("com.example.SuperChat").name == "SuperChat");
    EXPECT(resolve_app_style("com.example.chat.app").name == "Chat");
    EXPECT(resolve_app_style("").name == "Notification");
    EXPECT(resolve_app_style("", "org.example.pager").name == "Pager");

    // Unknown apps get a colour derived from their name: stable, and different names differ.
    const AppStyle first = resolve_app_style("Zorgon Chat");
    const AppStyle again = resolve_app_style("Zorgon Chat");
    EXPECT(first.accent.red == again.accent.red && first.accent.green == again.accent.green &&
           first.accent.blue == again.accent.blue);
    bool any_difference = false;
    for (const char* other : {"Alpha", "Bravo", "Charlie", "Delta", "Echo", "Foxtrot"}) {
        const AppStyle style = resolve_app_style(other);
        any_difference = any_difference || style.accent.red != first.accent.red ||
                         style.accent.green != first.accent.green ||
                         style.accent.blue != first.accent.blue;
    }
    EXPECT(any_difference);

    EXPECT(resolve_app_style("\xC3\x85pp").monogram == "\xC3\x85");
    EXPECT(resolve_app_style("\xC3\xA5pp").monogram == "\xC3\x85");  // lower-case a-ring
    EXPECT(resolve_app_style("[beta] app").monogram == "B");

    EXPECT(looks_like_bundle_id("io.heckel.ntfy"));
    EXPECT(!looks_like_bundle_id("Messages"));
    EXPECT(!looks_like_bundle_id("Mr. Smith"));
    EXPECT(!looks_like_bundle_id(".hidden"));
    EXPECT(!looks_like_bundle_id("Z\xC3\xBC" "rich.ch"));
}

void test_time_format() {
    EXPECT(days_from_civil(1970, 1, 1) == 0);
    EXPECT(days_from_civil(2000, 1, 1) == 10957);
    EXPECT(days_from_civil(2026, 9, 30) == 20726);
    EXPECT(days_from_civil(2024, 2, 29) == 19782);
    EXPECT(days_from_civil(1969, 12, 31) == -1);

    const CivilTime epoch = civil_from_epoch(0, 0);
    EXPECT(epoch.year == 1970 && epoch.month == 1 && epoch.day == 1 && epoch.weekday == 4);
    const CivilTime before = civil_from_epoch(-1, 0);
    EXPECT(before.year == 1969 && before.month == 12 && before.day == 31 && before.hour == 23 &&
           before.minute == 59 && before.second == 59 && before.weekday == 3);
    const CivilTime leap = civil_from_epoch(19782 * 86400LL + 13 * 3600 + 5 * 60 + 9, 0);
    EXPECT(leap.year == 2024 && leap.month == 2 && leap.day == 29 && leap.hour == 13 &&
           leap.minute == 5 && leap.second == 9 && leap.weekday == 4);
    const CivilTime shifted = civil_from_epoch(20726 * 86400LL + 23 * 3600, 2 * 3600);
    EXPECT(shifted.day == 1 && shifted.month == 10 && shifted.hour == 1 && shifted.weekday == 4);

    constexpr int kOffset = 2 * 3600;
    const std::int64_t now = 1790754060;  // 2026-09-30 09:41 at UTC+2, a Wednesday
    EXPECT(civil_from_epoch(now, kOffset).hour == 9 && civil_from_epoch(now, kOffset).minute == 41);
    EXPECT(day_label(now, now - 60, kOffset) == "Today");
    EXPECT(day_label(now, now - 9 * 3600 - 42 * 60, kOffset) == "Yesterday");  // 23:59 the day before
    EXPECT(day_label(now, now - 3 * 86400, kOffset) == "Sunday");
    EXPECT(day_label(now, now - 9 * 86400, kOffset) == "21 Sep");
    EXPECT(day_label(now, now - 400 * 86400, kOffset) == "26 Aug 2025");
    EXPECT(day_label(now, now + 500, kOffset) == "Today");
    // 08:10 UTC and the 07:50 UTC notification before it: the same day at UTC+2, but on either side
    // of local midnight at UTC-8.
    const std::int64_t later = now + 29 * 60;
    EXPECT(day_label(later, later - 20 * 60, kOffset) == "Today");
    EXPECT(day_label(later, later - 20 * 60, -8 * 3600) == "Yesterday");

    EXPECT(notification_time_label(now, now - 10, kOffset) == "Just now");
    EXPECT(notification_time_label(now, now + 30, kOffset) == "Just now");
    EXPECT(notification_time_label(now, now - 5 * 60, kOffset) == "5 min ago");
    EXPECT(notification_time_label(now, now - 59 * 60, kOffset) == "59 min ago");
    EXPECT(notification_time_label(now, now - 2 * 3600, kOffset) == "07:41");
    EXPECT(notification_time_label(now, now - 30 * 3600, kOffset) == "03:41");

    EXPECT(local_day_number(now, kOffset) == 20726);
    const int offset = local_utc_offset_seconds(now);
    EXPECT(offset >= -14 * 3600 && offset <= 14 * 3600);
}

void test_iso8601() {
    const std::int64_t expected = days_from_civil(2026, 9, 29) * 86400 + 15 * 3600 + 2 * 60;  // 15:02Z
    EXPECT(parse_iso8601("2026-09-29T17:02:00+02:00", 0) == expected);
    EXPECT(parse_iso8601("2026-09-29T15:02:00Z", 0) == expected);
    EXPECT(parse_iso8601("2026-09-29t15:02:00z", 0) == expected);
    EXPECT(parse_iso8601("2026-09-29T17:02:00+0200", 0) == expected);
    EXPECT(parse_iso8601("2026-09-29T17:02:00+02", 0) == expected);
    EXPECT(parse_iso8601("2026-09-29 17:02:00+02:00", 0) == expected);
    EXPECT(parse_iso8601("2026-09-29T17:02+02:00", 0) == expected);
    EXPECT(parse_iso8601("2026-09-29T17:02:00.123456+02:00", 0) == expected);
    EXPECT(parse_iso8601("2026-09-29T09:32:00-05:30", 0) == expected);
    // Without a zone the sender's clock is taken to be in the default zone.
    EXPECT(parse_iso8601("2026-09-29T17:02:00", 7200) == expected);
    EXPECT(parse_iso8601("2026-09-29T17:02:00", 0) == expected + 2 * 3600);
    EXPECT(parse_iso8601("2024-02-29T00:00:00Z", 0) == days_from_civil(2024, 2, 29) * 86400);

    for (const char* invalid :
         {"", "now", "17:02", "2026-09-29", "2026-13-01T00:00:00Z", "2026-00-10T00:00:00Z",
          "2026-02-30T00:00:00Z", "2025-02-29T00:00:00Z", "2026-09-29T24:00:00Z",
          "2026-09-29T17:60:00Z", "2026-09-29T17:02:60Z", "2026-09-29T17:02:00+25:00",
          "2026-09-29T17:02:00+02:0", "2026-09-29T17:02:00Zjunk", "2026-09-29T17:02:00.Z",
          "1969-12-31T23:59:59Z", "2026-09-29X17:02:00Z", "2026/09/29 17:02:00"}) {
        EXPECT(!parse_iso8601(invalid, 0).has_value());
    }
}

void test_time_zones() {
#ifndef _WIN32
    // POSIX rules, so the test does not depend on the machine having tz data.
    const std::int64_t september = 1790754060;                        // 2026-09-30 07:41 UTC
    const std::int64_t january = days_from_civil(2026, 1, 15) * 86400 + 12 * 3600;
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
    EXPECT(local_utc_offset_seconds(september) == 7200);             // summer time
    EXPECT(local_utc_offset_seconds(january) == 3600);               // winter time
    EXPECT(describe_local_time(september) == "2026-09-30 09:41:00 (UTC+02:00)");
    EXPECT(describe_local_time(january) == "2026-01-15 13:00:00 (UTC+01:00)");
    setenv("TZ", "UTC0", 1);
    tzset();
    EXPECT(local_utc_offset_seconds(september) == 0);
    EXPECT(describe_local_time(september) == "2026-09-30 07:41:00 (UTC+00:00)");
    setenv("TZ", "EST5EDT,M3.2.0,M11.1.0", 1);
    tzset();
    EXPECT(local_utc_offset_seconds(september) == -4 * 3600);
    EXPECT(describe_local_time(september) == "2026-09-30 03:41:00 (UTC-04:00)");
    setenv("TZ", "IST-5:30", 1);
    tzset();
    EXPECT(describe_local_time(september) == "2026-09-30 13:11:00 (UTC+05:30)");
    unsetenv("TZ");
    tzset();
#endif
}

// Drives a controller the way the dashboard does: events, then a 25 ms tick.
void frame(ScrollController& scroll, int count = 1) {
    for (int index = 0; index < count; ++index) scroll.tick(0.025F);
}

void test_scroll_controller() {
    ScrollController scroll;
    scroll.set_maximum(1000.0F);

    // A press that barely moves is a tap and scrolls nothing.
    scroll.press(300.0F);
    EXPECT(scroll.pressed() && !scroll.dragging());
    scroll.move(322.0F);                                           // 22 px: still within a wobbly tap
    frame(scroll);
    EXPECT(!scroll.dragging() && scroll.position() == 0.0F);
    EXPECT(scroll.release());
    EXPECT(!scroll.pressed());
    EXPECT(!scroll.release());                                     // nothing is pressed any more

    // Dragging up moves further down the content, one to one, without a jump at the threshold.
    scroll.press(400.0F);
    scroll.move(370.0F);                                           // passes the threshold: anchor
    EXPECT(scroll.dragging() && scroll.position() == 0.0F);
    scroll.move(330.0F);
    EXPECT(std::fabs(scroll.position() - 40.0F) < 0.001F);
    scroll.move(480.0F);                                           // back below the anchor: top
    EXPECT(scroll.position() == 0.0F);
    scroll.move(-10000.0F);
    EXPECT(scroll.position() == 1000.0F);                          // clamped at the end
    EXPECT(!scroll.release());                                     // a drag is never a tap
    frame(scroll, 5);
    EXPECT(scroll.position() == 1000.0F && scroll.velocity() == 0.0F);  // still held: no fling

    // Letting go while moving fast keeps the list going, slowing to a stop inside the content.
    scroll.reset();
    scroll.set_maximum(5000.0F);
    scroll.press(700.0F);
    scroll.move(670.0F);
    float pointer = 670.0F;
    for (int step = 0; step < 12; ++step) {
        pointer -= 40.0F;                                          // 1600 px/s
        scroll.move(pointer);
        frame(scroll);
    }
    const float at_release = scroll.position();
    EXPECT(!scroll.release());
    EXPECT(scroll.velocity() > 1000.0F && scroll.velocity() < 2000.0F);
    frame(scroll, 4);
    EXPECT(scroll.position() > at_release + 100.0F);
    float previous = scroll.position();
    for (int step = 0; step < 200 && scroll.velocity() != 0.0F; ++step) {
        frame(scroll);
        EXPECT(scroll.position() >= previous);
        previous = scroll.position();
    }
    EXPECT(scroll.velocity() == 0.0F);
    EXPECT(scroll.position() > at_release + 300.0F && scroll.position() < 5000.0F);
    const float rest = scroll.position();
    frame(scroll, 10);
    EXPECT(scroll.position() == rest);

    // Touching a moving list stops it instead of clicking whatever is underneath.
    scroll.reset();
    scroll.press(600.0F);
    scroll.move(570.0F);
    for (int step = 0; step < 10; ++step) {
        scroll.move(570.0F - 40.0F * static_cast<float>(step + 1));
        frame(scroll);
    }
    EXPECT(!scroll.release() && scroll.velocity() != 0.0F);
    scroll.press(300.0F);
    EXPECT(scroll.velocity() == 0.0F);
    const float stopped_at = scroll.position();
    frame(scroll, 5);
    EXPECT(scroll.position() == stopped_at);
    EXPECT(!scroll.release());                                     // that touch was not a tap
    scroll.press(300.0F);
    EXPECT(scroll.release());                                      // the next one is

    // Pausing before letting go means no fling.
    scroll.reset();
    scroll.press(600.0F);
    scroll.move(570.0F);
    for (int step = 0; step < 8; ++step) {
        scroll.move(570.0F - 30.0F * static_cast<float>(step + 1));
        frame(scroll);
    }
    frame(scroll, 6);                                              // 150 ms without moving
    const float paused_at = scroll.position();
    static_cast<void>(scroll.release());
    EXPECT(scroll.velocity() == 0.0F);
    frame(scroll, 10);
    EXPECT(scroll.position() == paused_at);

    // Flinging into the end stops there.
    scroll.reset();
    scroll.set_maximum(300.0F);
    scroll.press(700.0F);
    scroll.move(670.0F);
    pointer = 670.0F;
    for (int step = 0; step < 8; ++step) {
        pointer -= 50.0F;
        scroll.move(pointer);
        frame(scroll);
    }
    static_cast<void>(scroll.release());
    frame(scroll, 100);
    EXPECT(scroll.position() == 300.0F && scroll.velocity() == 0.0F);

    // The wheel glides smoothly to its target, and cancelling ends a drag without a tap.
    scroll.reset();
    scroll.set_maximum(1000.0F);
    scroll.scroll_by(300.0F);
    float last = 0.0F;
    bool monotonic = true;
    for (int step = 0; step < 80; ++step) {
        frame(scroll);
        monotonic = monotonic && scroll.position() >= last;
        last = scroll.position();
    }
    EXPECT(monotonic && scroll.position() == 300.0F);
    scroll.scroll_by(-5000.0F);
    frame(scroll, 200);
    EXPECT(scroll.position() == 0.0F);
    scroll.scroll_by(99999.0F);
    frame(scroll, 200);
    EXPECT(scroll.position() == 1000.0F);

    scroll.press(100.0F);
    scroll.move(40.0F);
    scroll.cancel();
    EXPECT(!scroll.pressed() && !scroll.dragging());
    scroll.press(100.0F);
    scroll.cancel();                                               // a cancelled press is no tap
    EXPECT(!scroll.release());

    // The limit can shrink underneath the current position.
    scroll.set_maximum(400.0F);
    EXPECT(scroll.position() == 400.0F);
    scroll.set_maximum(-5.0F);
    EXPECT(scroll.position() == 0.0F);
    scroll.tick(10.0F);                                            // absurd frame times are bounded
    EXPECT(scroll.position() == 0.0F);

    scroll.set_maximum(1000.0F);
    scroll.scroll_by(500.0F);
    scroll.jump_to(250.0F);                                        // also ends any glide
    frame(scroll, 50);
    EXPECT(scroll.position() == 250.0F && scroll.velocity() == 0.0F);
    scroll.jump_to(99999.0F);
    EXPECT(scroll.position() == 1000.0F);
}

void test_canvas() {
    Canvas canvas(64, 64, {10, 20, 30, 255});
    canvas.fill_rounded_rect(7.5F, 8, 56, 56, 12, {255, 0, 0, 255});
    const auto pixel = [&](int x, int y) {
        const auto* data = &canvas.pixels()[(static_cast<std::size_t>(y) * 64U + static_cast<std::size_t>(x)) * 4U];
        return std::vector<int>{data[0], data[1], data[2], data[3]};
    };
    EXPECT(pixel(32, 32) == (std::vector<int>{255, 0, 0, 255}));   // solid inside
    EXPECT(pixel(2, 2) == (std::vector<int>{10, 20, 30, 255}));    // untouched outside
    const auto corner = pixel(9, 9);                               // inside the rounded corner
    EXPECT(corner[0] == 10 && corner[1] == 20);                    // cut away by the radius
    const auto edge = pixel(7, 32);                                // the edge passes through its centre
    EXPECT(edge[0] > 100 && edge[0] < 200);                        // half covered, so half red
    EXPECT(canvas.pixels()[3] == 255);                             // opacity is preserved

    Canvas transparent(8, 8, {0, 0, 0, 0});
    transparent.fill_rect(2, 2, 6, 6, {200, 100, 50, 128});
    const auto* middle = &transparent.pixels()[(4U * 8U + 4U) * 4U];
    EXPECT(middle[0] == 200 && middle[1] == 100 && middle[2] == 50 && middle[3] >= 127 && middle[3] <= 129);
    EXPECT(transparent.pixels()[3] == 0);

    Canvas text(200, 60, {0, 0, 0, 255});
    const float advance = text.draw_text(10, 40, "Hi", {32.0F, FontWeight::kSemiBold, 0.0F}, {255, 255, 255, 255});
    EXPECT(advance > 20.0F);
    std::uint32_t bright = 0;
    for (std::size_t index = 0; index < text.pixels().size(); index += 4) bright += text.pixels()[index] > 128 ? 1U : 0U;
    EXPECT(bright > 80U);
    Canvas right(200, 60, {0, 0, 0, 255});
    right.draw_text(190, 40, "Hi", {32.0F, FontWeight::kSemiBold, 0.0F}, {255, 255, 255, 255}, TextAlign::kRight);
    std::uint32_t right_side = 0;
    std::uint32_t left_side = 0;
    for (int y = 0; y < 60; ++y) {
        for (int x = 0; x < 200; ++x) {
            const bool lit = right.pixels()[(static_cast<std::size_t>(y) * 200U + static_cast<std::size_t>(x)) * 4U] > 128;
            (x >= 100 ? right_side : left_side) += lit ? 1U : 0U;
        }
    }
    EXPECT(right_side > 0U && left_side == 0U);
}

}  // namespace

int main() {
    test_rasterizer();
    test_font();
    test_typography();
    test_app_style();
    test_time_format();
    test_iso8601();
    test_time_zones();
    test_scroll_controller();
    test_canvas();
    if (failures != 0) {
        std::cerr << failures << " expectation(s) failed\n";
        return 1;
    }
    return 0;
}
