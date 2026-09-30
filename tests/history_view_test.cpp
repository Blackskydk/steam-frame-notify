#include "ui/history_view.h"
#include "ui/typography.h"

#include <cstddef>
#include <cstdint>
#include <iostream>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

using namespace frame_notify::ui;

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "history_view_test.cpp:" << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(condition) expect((condition), #condition, __LINE__)

std::uint64_t checksum(const std::vector<std::uint8_t>& pixels) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const auto byte : pixels) {
        value ^= byte;
        value *= 1099511628211ULL;
    }
    return value;
}

std::size_t bright_pixels(const std::vector<std::uint8_t>& pixels) {
    std::size_t count = 0;
    for (std::size_t index = 0; index < pixels.size(); index += 4) {
        if (pixels[index] > 180 || pixels[index + 1] > 180 || pixels[index + 2] > 180) ++count;
    }
    return count;
}

constexpr std::int64_t kNow = 1790754060;  // 2026-09-30 09:41 at UTC+2

HistoryNotification make(const std::string& id, const std::string& app, const std::string& title,
                         const std::string& message, std::int64_t age, bool read,
                         const std::string& app_id = {}) {
    HistoryNotification notification;
    notification.id = id;
    notification.app = app;
    notification.app_id = app_id;
    notification.title = title;
    notification.message = message;
    notification.read = read;
    notification.received_at = kNow - age;
    return notification;
}

std::vector<HistoryNotification> sample() {
    const std::string long_message =
        "Are you coming home soon? I made dinner and it is getting cold, so hurry up before "
        "everything is ruined and the neighbours start to complain about the smell. Also please "
        "pick up milk and bread on the way, and do not forget the parcel from the post office.";
    return {
        make("1", "Messages", "Jane Doe", long_message, 180, false, "com.apple.MobileSMS"),
        make("2", "ntfy", "Frame Notify test", "Test message", 20 * 60, false, "io.heckel.ntfy"),
        make("3", "com.hammerandchisel.discord", "General", "Someone mentioned you", 3 * 3600, true),
        make("4", "Calendar", "Design review", "Starts in 15 minutes", 26 * 3600, true),
        make("5", "Weather", "Rain tomorrow", "Remember your rain jacket", 9 * 86400, true),
    };
}

HistoryContext context(std::string expanded = {}) {
    HistoryContext value;
    value.now = kNow;
    value.utc_offset_seconds = 2 * 3600;
    value.expanded_id = std::move(expanded);
    return value;
}

HistoryContext context_with_phone(std::string state,
                                  std::map<std::string, std::string> fields = {}) {
    HistoryContext value = context();
    value.phone.state = std::move(state);
    value.phone.fields = std::move(fields);
    return value;
}

// The columns of the header row that answer to a hit test, per kind.
struct HeaderSpan {
    int first = -1;
    int last = -1;
    [[nodiscard]] bool found() const { return first >= 0; }
};

HeaderSpan header_span(const HistoryView& view, HistoryHitKind kind, int row = 78) {
    HeaderSpan span;
    for (int x = 0; x < static_cast<int>(kHistoryViewWidth); ++x) {
        const auto hit = view.hit_test(x, row, 0);
        if (hit && hit->kind == kind) {
            if (!span.found()) span.first = x;
            span.last = x;
        }
    }
    return span;
}

}  // namespace

int main() {
    EXPECT(Typography::shared().has_fonts());

    HistoryView view;
    view.set(sample(), context());
    const int viewport = static_cast<int>(kHistoryViewHeight);
    const std::size_t viewport_bytes = static_cast<std::size_t>(kHistoryViewWidth) * kHistoryViewHeight *
                                       kHistoryViewBytesPerPixel;
    EXPECT(view.content_height() > viewport);
    EXPECT(view.maximum_scroll_offset() == view.content_height() - viewport);

    const auto first_page = view.render_viewport(0);
    const auto last_page = view.render_viewport(view.maximum_scroll_offset());
    const auto content = view.render_content();
    EXPECT(first_page.size() == viewport_bytes && last_page.size() == viewport_bytes);
    EXPECT(content.size() == static_cast<std::size_t>(kHistoryViewWidth) *
                                 static_cast<std::size_t>(view.content_height()) *
                                 kHistoryViewBytesPerPixel);
    EXPECT(checksum(first_page) != checksum(last_page));
    EXPECT(bright_pixels(first_page) > 5000U);
    EXPECT(view.render_viewport(-500) == first_page);                       // scroll is clamped
    EXPECT(view.render_viewport(1 << 20) == last_page);
    for (std::size_t index = 3; index < content.size(); index += 4) {       // fully opaque panel
        if (content[index] != 255) {
            EXPECT(false);
            break;
        }
    }

    // Layout: cards are ordered, separated, and tall enough for the avatar.
    int previous_bottom = 0;
    for (std::size_t index = 0; index < 5; ++index) {
        const auto span = view.card_span(index);
        EXPECT(span.has_value());
        if (!span) continue;
        EXPECT(span->first > previous_bottom && span->second - span->first >= 120);
        previous_bottom = span->second;
    }
    EXPECT(!view.card_span(5).has_value());
    EXPECT(view.card_span(0)->first > 150);                                 // below the header

    // Hit testing.
    const int top = view.card_span(0)->first;
    const auto body = view.hit_test(400, top + 60, 0);
    EXPECT(body && body->kind == HistoryHitKind::kCard && body->notification_index == 0U);
    const auto dismiss = view.hit_test(1180, top + 50, 0);
    EXPECT(dismiss && dismiss->kind == HistoryHitKind::kDismiss && dismiss->notification_index == 0U);
    const auto near_dismiss = view.hit_test(1180 + 30, top + 50 + 30, 0);
    EXPECT(near_dismiss && near_dismiss->kind == HistoryHitKind::kDismiss);
    EXPECT(view.hit_test(1180 - 60, top + 50, 0)->kind == HistoryHitKind::kCard);
    const auto clear_all = view.hit_test(1150, 78, 0);
    EXPECT(clear_all && clear_all->kind == HistoryHitKind::kClearAll);
    EXPECT(!view.hit_test(1150, 140, 0));                                   // header, no control
    EXPECT(!view.hit_test(20, top + 60, 0));                                // left margin
    EXPECT(!view.hit_test(1260, top + 60, 0));                              // right margin
    EXPECT(!view.hit_test(400, view.card_span(0)->second + 5, 0));          // gap between cards
    EXPECT(!view.hit_test(-1, 100, 0) && !view.hit_test(100, -1, 0) && !view.hit_test(100, viewport, 0));
    // The same content point is reached through different pointer rows once scrolled.
    const auto second_top = view.card_span(1)->first;
    const int scroll = second_top - 100;
    const auto scrolled = view.hit_test(400, 100 + 60, scroll);
    EXPECT(scrolled && scrolled->kind == HistoryHitKind::kCard && scrolled->notification_index == 1U);
    EXPECT(view.hit_test(400, 100 + 60, 1 << 20).has_value());              // clamped, still valid

    // The last card can be reached by scrolling to the end.
    const auto last_span = view.card_span(4);
    const auto last_hit = view.hit_test(400, last_span->first + 40 - view.maximum_scroll_offset(),
                                        view.maximum_scroll_offset());
    EXPECT(last_hit && last_hit->notification_index == 4U);

    // Expanding a long message makes its card taller, and only that card.
    HistoryView expanded;
    expanded.set(sample(), context("1"));
    const auto collapsed_span = view.card_span(0);
    const auto expanded_span = expanded.card_span(0);
    EXPECT(expanded_span->second - expanded_span->first > collapsed_span->second - collapsed_span->first);
    const auto short_collapsed = view.card_span(1);
    const auto short_expanded = expanded.card_span(1);
    EXPECT(short_expanded->second - short_expanded->first == short_collapsed->second - short_collapsed->first);
    EXPECT(expanded.content_height() > view.content_height());
    EXPECT(checksum(expanded.render_content()) != checksum(content));
    HistoryView unknown_expanded;
    unknown_expanded.set(sample(), context("no-such-id"));
    EXPECT(unknown_expanded.content_height() == view.content_height());

    // Expanding a short message changes nothing about its size.
    HistoryView short_message;
    short_message.set(sample(), context("2"));
    EXPECT(short_message.content_height() == view.content_height());

    // Read and unread cards look different.
    auto read_items = sample();
    for (auto& item : read_items) item.read = true;
    HistoryView all_read;
    all_read.set(read_items, context());
    EXPECT(all_read.content_height() == view.content_height());
    EXPECT(checksum(all_read.render_content()) != checksum(content));

    // A message that only repeats the title adds no line.
    HistoryView repeated;
    repeated.set({make("a", "Mail", "Same", "Same", 60, false)}, context());
    HistoryView distinct;
    distinct.set({make("a", "Mail", "Same", "Different", 60, false)}, context());
    const auto repeated_span = repeated.card_span(0);
    const auto distinct_span = distinct.card_span(0);
    EXPECT(repeated_span->second - repeated_span->first < distinct_span->second - distinct_span->first);

    // Notifications from different days get their own headings, which are not clickable. Cards 0-2
    // are from today, so the first heading after the opening one sits above card 3 ("Yesterday").
    EXPECT(view.card_span(3)->first - view.card_span(2)->second > 62);
    EXPECT(!view.hit_test(400, view.card_span(3)->first - 30, 0));
    EXPECT(view.card_span(1)->first - view.card_span(0)->second < 30);      // same day: no heading

    // Empty state.
    HistoryView empty;
    empty.set({}, context());
    EXPECT(empty.content_height() == viewport && empty.maximum_scroll_offset() == 0);
    EXPECT(bright_pixels(empty.render_viewport(0)) > 500U);
    EXPECT(!empty.hit_test(640, 300, 0));
    EXPECT(!empty.hit_test(640, 452 + 34, 0));                              // no phone to pair yet
    EXPECT(checksum(empty.render_viewport(0)) != checksum(first_page));

    // Many notifications stay within a sensible texture size, even with long messages.
    std::vector<HistoryNotification> many;
    for (int index = 0; index < 50; ++index) {
        many.push_back(make(std::to_string(index), "Messages", "Title " + std::to_string(index),
                            sample()[0].message, 60 + index * 3600, index % 3 != 0));
    }
    HistoryView crowded;
    crowded.set(many, context("3"));
    EXPECT(crowded.content_height() > viewport && crowded.content_height() <= 8192);
    EXPECT(crowded.hidden_count() > 0 && crowded.hidden_count() < 50);
    const auto shown = static_cast<std::size_t>(50 - crowded.hidden_count());
    EXPECT(crowded.card_span(shown - 1).has_value() && !crowded.card_span(shown).has_value());
    EXPECT(view.hidden_count() == 0 && empty.hidden_count() == 0);
    const auto hidden_hit = crowded.hit_test(400, viewport - 5, crowded.maximum_scroll_offset());
    EXPECT(!hidden_hit || hidden_hit->notification_index < shown);            // never an unseen card
    EXPECT(crowded.render_content().size() ==
           static_cast<std::size_t>(kHistoryViewWidth) * static_cast<std::size_t>(crowded.content_height()) *
               kHistoryViewBytesPerPixel);

    // Unusual text must not break layout or drawing.
    HistoryView odd;
    odd.set({make("x", "com.example.unknownapp", std::string(400, 'W'), std::string(3000, 'x'), 5, false),
             make("y", "", "", "", 5, true),
             make("z", "Mail", "Emoji \xF0\x9F\x98\x80", "K\xC3\xB8" "benhavn \xF0\x9F\x91\x8D\xF0\x9F\x8F\xBD \xE2\x80\x8D", 5, false)},
            context("x"));
    EXPECT(odd.content_height() > viewport);
    EXPECT(odd.render_viewport(0).size() == viewport_bytes);

    // A card shows the sender's own timestamp when it is a valid date-time, otherwise the moment it
    // arrived, and never a time in the future.
    const auto label_for = [&](const std::string& sent, std::int64_t arrived_ago) {
        auto item = make("t", "Mail", "Title", "Body", arrived_ago, false);
        item.time = sent;
        HistoryView timed;
        timed.set({item}, context());
        return timed.card_time_label(0).value_or("<missing>");
    };
    EXPECT(label_for("2026-09-30T09:11:00+02:00", 10) == "30 min ago");      // sender says 30 min ago
    EXPECT(label_for("2026-09-30T07:11:00Z", 10) == "30 min ago");
    EXPECT(label_for("2026-09-30T09:11:00", 10) == "30 min ago");            // no zone: local time
    EXPECT(label_for("2026-09-29T20:15:00+02:00", 10) == "20:15");           // yesterday evening
    EXPECT(label_for("now", 10) == "Just now");                              // not a date: use arrival
    EXPECT(label_for("", 5 * 60) == "5 min ago");
    EXPECT(label_for("17:02", 7 * 60) == "7 min ago");
    EXPECT(label_for("2026-09-30T23:00:00+02:00", 10) == "Just now");        // the future is clamped
    EXPECT(view.card_time_label(0) == std::optional<std::string>("3 min ago"));
    EXPECT(view.card_time_label(2) == std::optional<std::string>("06:41"));  // 3 h ago at UTC+2
    EXPECT(!view.card_time_label(99).has_value());

    // The signature tells whether a re-render would change a single pixel.
    HistoryView same;
    same.set(sample(), context());
    EXPECT(same.signature() == view.signature() && view.signature() != 0U);
    // Deterministic no matter what the heap looks like (it once hashed uninitialised memory).
    for (int round = 0; round < 25; ++round) {
        std::vector<std::vector<char>> noise;
        for (int fill = 0; fill < round * 3; ++fill) noise.emplace_back(static_cast<std::size_t>(40 + fill * 7), 'x');
        HistoryView fresh;
        fresh.set(sample(), context());
        EXPECT(fresh.signature() == view.signature());
    }
    HistoryContext minute_later = context();
    minute_later.now += 60;
    HistoryView later;
    later.set(sample(), minute_later);                                       // "3 min" becomes "4 min"
    EXPECT(later.signature() != view.signature());
    std::vector<HistoryNotification> old_only = {sample()[2], sample()[3], sample()[4]};
    HistoryView old_before;
    old_before.set(old_only, context());
    HistoryView old_after;
    old_after.set(old_only, minute_later);                                   // clock times: unchanged
    EXPECT(old_before.signature() == old_after.signature());
    auto toggled = sample();
    toggled[0].read = true;
    HistoryView toggled_view;
    toggled_view.set(toggled, context());
    EXPECT(toggled_view.signature() != view.signature());
    EXPECT(expanded.signature() != view.signature());
    EXPECT(empty.signature() != view.signature());

    // The phone's status sits in the header, left of "Clear all", and opens the phone screen.
    {
        const HeaderSpan chip = header_span(view, HistoryHitKind::kPhoneChip);
        const HeaderSpan clear = header_span(view, HistoryHitKind::kClearAll);
        EXPECT(chip.found() && clear.found());
        EXPECT(chip.last < clear.first);                                     // they never overlap
        EXPECT(clear.first - chip.last < 40);                                // and sit side by side
        EXPECT(chip.last - chip.first > 150);                                // wide enough to tap
        // The gear sits at the right edge, after "Clear all", without overlapping anything.
        const HeaderSpan gear = header_span(view, HistoryHitKind::kSettings);
        EXPECT(gear.found() && clear.last < gear.first && gear.last > 1200 && gear.last < 1250);
        EXPECT(gear.last - gear.first > 40);                                 // big enough to hit with a controller
        EXPECT(view.hit_test((gear.first + gear.last) / 2, 70, 0)->kind == HistoryHitKind::kSettings);
        EXPECT(!view.hit_test(gear.last + 30, 78, 0));                       // nothing beyond it
        EXPECT(view.hit_test(chip.first, 78, 0)->kind == HistoryHitKind::kPhoneChip);
        EXPECT(!view.hit_test(chip.first - 40, 78, 0));                      // empty header beside it
        EXPECT(view.hit_test((chip.first + chip.last) / 2, 70, 0)->kind == HistoryHitKind::kPhoneChip);
        EXPECT(!view.hit_test((chip.first + chip.last) / 2, 140, 0));        // below it, in the header
        // The header scrolls away with the cards, and with it the chip.
        EXPECT(!view.hit_test((chip.first + chip.last) / 2, 70, 300) ||
               view.hit_test((chip.first + chip.last) / 2, 70, 300)->kind != HistoryHitKind::kPhoneChip);
    }

    // What the chip says follows the phone, and the picture changes with it.
    {
        HistoryView unpaired;
        unpaired.set(sample(), context_with_phone("unpaired"));
        HistoryView connected;
        connected.set(sample(), context_with_phone("connected", {{"phone", "iPhone"}}));
        HistoryView other_phone;
        other_phone.set(sample(), context_with_phone("connected", {{"phone", "Pixel"}}));
        HistoryView connecting;
        connecting.set(sample(), context_with_phone("connecting"));
        HistoryView helper_down;
        helper_down.set(sample(), context_with_phone("helper_unavailable", {{"message", "x"}}));
        EXPECT(unpaired.signature() != connected.signature());
        EXPECT(connected.signature() != other_phone.signature());            // the name is in the label
        EXPECT(connecting.signature() != connected.signature());
        EXPECT(helper_down.signature() != unpaired.signature());
        EXPECT(checksum(unpaired.render_viewport(0)) != checksum(connected.render_viewport(0)));
        EXPECT(checksum(connected.render_viewport(0)) != checksum(other_phone.render_viewport(0)));
        EXPECT(unpaired.content_height() == connected.content_height());     // the chip never moves cards
        for (const HistoryView* each : {&unpaired, &connected, &other_phone, &connecting, &helper_down}) {
            EXPECT(header_span(*each, HistoryHitKind::kPhoneChip).found());
            EXPECT(header_span(*each, HistoryHitKind::kClearAll).found());
        }
        // A long phone name is cut short instead of pushing "Clear all" off the panel.
        HistoryView long_name;
        long_name.set(sample(), context_with_phone("connected", {{"phone", std::string(200, 'M')}}));
        const HeaderSpan long_chip = header_span(long_name, HistoryHitKind::kPhoneChip);
        EXPECT(long_chip.found() && long_chip.first > 200);
        EXPECT(long_chip.last < header_span(long_name, HistoryHitKind::kClearAll).first);
    }

    // With nothing to show, the status moves to the right edge, and "Pair an iPhone" is offered
    // only while no phone is paired.
    {
        HistoryView unpaired_empty;
        unpaired_empty.set({}, context_with_phone("unpaired"));
        const auto prompt = unpaired_empty.hit_test(640, 452 + 34, 0);
        EXPECT(prompt && prompt->kind == HistoryHitKind::kPairPrompt);
        EXPECT(unpaired_empty.hit_test(640 - 160, 452 + 34, 0)->kind == HistoryHitKind::kPairPrompt);
        EXPECT(!unpaired_empty.hit_test(640 - 400, 452 + 34, 0));
        EXPECT(!unpaired_empty.hit_test(640, 452 - 40, 0));
        EXPECT(!unpaired_empty.hit_test(640, 452 + 68 + 40, 0));
        EXPECT(452 + 68 < viewport * 2 / 3);                                   // well clear of the lower edge
        const HeaderSpan empty_chip = header_span(unpaired_empty, HistoryHitKind::kPhoneChip);
        EXPECT(empty_chip.found() && empty_chip.last > 1100);
        const HeaderSpan empty_gear = header_span(unpaired_empty, HistoryHitKind::kSettings);
        EXPECT(empty_gear.found() && empty_chip.last < empty_gear.first);   // the gear stays with nothing to clear
        EXPECT(!header_span(unpaired_empty, HistoryHitKind::kClearAll).found());

        for (const char* state : {"connected", "connecting", "needs_repair", "no_bluetooth", "pair_open",
                                  "pair_confirm", "helper_unavailable", "starting"}) {
            HistoryView quiet;
            quiet.set({}, context_with_phone(state));
            EXPECT(!quiet.hit_test(640, 452 + 34, 0));
            EXPECT(header_span(quiet, HistoryHitKind::kPhoneChip).found());
            EXPECT(checksum(quiet.render_viewport(0)) != checksum(unpaired_empty.render_viewport(0)));
        }
        HistoryView connected_empty;
        connected_empty.set({}, context_with_phone("connected", {{"phone", "iPhone"}}));
        EXPECT(unpaired_empty.signature() != connected_empty.signature());
    }

    if (failures != 0) {
        std::cerr << failures << " expectation(s) failed\n";
        return 1;
    }
    return 0;
}
