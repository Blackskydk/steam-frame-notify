#include "ui/history_view.h"

#include "ui/fingerprint.h"
#include "ui/icon.h"
#include "ui/renderer.h"
#include "ui/theme.h"
#include "ui/time_format.h"
#include "ui/typography.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <string>
#include <string_view>

namespace frame_notify::ui {
using namespace theme;

namespace {

// ---- Geometry (pixels) ---------------------------------------------------------------------
constexpr int kMargin = 48;
constexpr int kHeaderHeight = 156;
constexpr int kDayHeight = 62;
constexpr int kCardGap = 14;
constexpr int kFooterHeight = 100;
constexpr int kMaximumContentHeight = 8192;
constexpr int kCardRadius = 26;
constexpr int kCardPaddingX = 28;
constexpr int kAvatarTop = 26;
constexpr int kAvatarSize = 68;
constexpr int kTextOffset = kCardPaddingX + kAvatarSize + 24;   // from the card's left edge
constexpr int kDismissDiameter = 48;
constexpr int kTextRightInset = kCardPaddingX + kDismissDiameter + 24;
constexpr int kDismissHitRadius = 36;
constexpr int kAppBaseline = 46;
constexpr int kTitleBaseline = 86;
constexpr int kFirstLineBaseline = 124;
constexpr int kLineHeight = 32;
constexpr int kCardBottomPadding = 30;
constexpr int kMinimumCardHeight = kAvatarTop * 2 + kAvatarSize;
constexpr int kCollapsedLines = 2;
constexpr int kExpandedLines = 12;
constexpr int kClearButtonWidth = 172;
constexpr int kClearButtonHeight = 52;
constexpr int kClearButtonTop = 52;
constexpr int kClearButtonHitPadding = 10;
constexpr int kChipHeight = 52;
constexpr int kChipHitPadding = 6;  // less than the gap, so it never overlaps "Clear all"
constexpr int kChipGap = 16;
constexpr int kChipMaximumText = 330;
constexpr int kPromptWidth = 330;
constexpr int kPromptHeight = 68;
constexpr int kPromptTop = 452;  // high enough to stay clear of the panel's lower edge

// ---- Type ----------------------------------------------------------------------------------
constexpr TextStyle kTitleFont{44.0F, FontWeight::kSemiBold, 0.0F};
constexpr TextStyle kSubtitleFont{22.0F, FontWeight::kRegular, 0.0F};
constexpr TextStyle kDayFont{17.0F, FontWeight::kSemiBold, 1.8F};
constexpr TextStyle kAppFont{19.0F, FontWeight::kSemiBold, 0.3F};
constexpr TextStyle kTimeFont{19.0F, FontWeight::kRegular, 0.0F};
constexpr TextStyle kCardTitleFont{27.0F, FontWeight::kSemiBold, 0.0F};
constexpr TextStyle kMessageFont{23.0F, FontWeight::kRegular, 0.0F};
constexpr TextStyle kButtonFont{20.0F, FontWeight::kSemiBold, 0.0F};
constexpr TextStyle kPairButtonFont{26.0F, FontWeight::kSemiBold, 0.0F};
constexpr TextStyle kMonogramFont{32.0F, FontWeight::kSemiBold, 0.0F};
constexpr TextStyle kFooterFont{17.0F, FontWeight::kRegular, 0.0F};
constexpr TextStyle kEmptyTitleFont{34.0F, FontWeight::kSemiBold, 0.0F};
constexpr TextStyle kEmptyBodyFont{22.0F, FontWeight::kRegular, 0.0F};

std::string uppercase_ascii(std::string text) {
    for (char& character : text) {
        character = static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
    }
    return text;
}

int text_width() {
    return static_cast<int>(kHistoryViewWidth) - 2 * kMargin - kTextOffset - kTextRightInset;
}

// Centre of a card's round dismiss button for a card whose top edge is at `card_top`.
Point dismiss_center(int card_top) {
    return {static_cast<float>(static_cast<int>(kHistoryViewWidth) - kMargin - kCardPaddingX -
                               kDismissDiameter / 2),
            static_cast<float>(card_top + kAvatarTop + kDismissDiameter / 2)};
}

int clear_button_left() {
    return static_cast<int>(kHistoryViewWidth) - kMargin - kClearButtonWidth;
}

// What the empty panel says, which depends on whether there is a phone to wait for.
struct EmptyState {
    const char* subtitle;
    const char* title;
    const char* body;
    bool prompt;   // offer the "Pair an iPhone" button
};

EmptyState empty_state_for(const PhoneInfo& phone) {
    const std::string& state = phone.state;
    if (state == "unpaired") {
        return {"No phone paired", "Pair your iPhone", "Its notifications will show up here.", true};
    }
    if (state == "connected") {
        return {"All caught up", "You\xE2\x80\x99re all caught up",
                "Notifications from your iPhone will show up here.", false};
    }
    if (phone_state_is_pairing(state)) {
        return {"Pairing in progress", "Pairing your iPhone", "Tap the status at the top to continue.", false};
    }
    return {"Waiting for your iPhone", "Waiting for your iPhone",
            "Notifications will show up here once it connects. Tap the status at the top for details.", false};
}

// When a notification happened: the sender's own timestamp when it is a valid date-time (never in
// the future), otherwise the moment Frame Notify received it.
std::int64_t notification_stamp(const HistoryNotification& notification,
                                const HistoryContext& context) {
    if (const auto sent = parse_iso8601(notification.time, context.utc_offset_seconds)) {
        return std::min(*sent, context.now);
    }
    return notification.received_at > 0 ? notification.received_at : context.now;
}

}  // namespace

int HistoryView::maximum_scroll_offset() const noexcept {
    return std::max(0, content_height_ - static_cast<int>(kHistoryViewHeight));
}

void HistoryView::set(std::vector<HistoryNotification> notifications, HistoryContext context) {
    notifications_ = std::move(notifications);
    context_ = std::move(context);
    rows_.clear();
    hidden_count_ = 0;

    const Typography& typography = Typography::shared();
    const auto width = static_cast<float>(text_width());
    int y = kHeaderHeight;
    bool has_day = false;
    std::int64_t current_day = 0;

    for (std::size_t index = 0; index < notifications_.size(); ++index) {
        const HistoryNotification& notification = notifications_[index];
        const std::int64_t stamp = notification_stamp(notification, context_);
        const std::int64_t day = local_day_number(stamp, context_.utc_offset_seconds);
        const bool new_day = !has_day || day != current_day;
        const int heading_height = new_day ? kDayHeight : 0;

        Row card;
        card.index = index;
        card.top = y + heading_height;
        card.read = notification.read;
        card.style = resolve_app_style(notification.app, notification.app_id);
        card.title = typography.ellipsize(notification.title, kCardTitleFont, width);
        card.time_label = notification_time_label(context_.now, stamp, context_.utc_offset_seconds);
        card.expanded = !context_.expanded_id.empty() && context_.expanded_id == notification.id;

        // A message that only repeats the title adds nothing.
        if (!notification.message.empty() && notification.message != notification.title) {
            const auto preview = typography.wrap(notification.message, kMessageFont, width,
                                                 kCollapsedLines + 1);
            card.expandable = static_cast<int>(preview.size()) > kCollapsedLines;
            card.lines = typography.wrap(notification.message, kMessageFont, width,
                                         card.expanded ? kExpandedLines : kCollapsedLines);
        }
        const int last_baseline = card.lines.empty()
                                      ? kTitleBaseline
                                      : kFirstLineBaseline +
                                            (static_cast<int>(card.lines.size()) - 1) * kLineHeight;
        card.height = std::max(kMinimumCardHeight, last_baseline + kCardBottomPadding);

        // Stop before the panel would outgrow the largest texture it may produce; the first card
        // is always shown.
        if (!rows_.empty() && y + heading_height + card.height + kFooterHeight > kMaximumContentHeight) {
            hidden_count_ = static_cast<int>(notifications_.size() - index);
            break;
        }
        if (new_day) {
            Row heading;
            heading.is_day = true;
            heading.top = y;
            heading.height = kDayHeight;
            heading.label = day_label(context_.now, stamp, context_.utc_offset_seconds);
            rows_.push_back(std::move(heading));
            current_day = day;
            has_day = true;
        }
        y += heading_height + card.height + kCardGap;
        rows_.push_back(std::move(card));
    }

    content_height_ = notifications_.empty()
                          ? static_cast<int>(kHistoryViewHeight)
                          : std::max(static_cast<int>(kHistoryViewHeight), y + kFooterHeight - kCardGap);

    // The phone status sits left of "Clear all", or at the right edge when that button is hidden.
    const PhoneChip chip = phone_chip(context_.phone);
    chip_label_ = typography.ellipsize(chip.label, kButtonFont, static_cast<float>(kChipMaximumText));
    chip_tone_ = chip.tone;
    const int chip_width = static_cast<int>(std::lround(typography.measure(chip_label_, kButtonFont))) +
                           26 + 12 + 12 + 28;
    const int chip_right = notifications_.empty()
                               ? static_cast<int>(kHistoryViewWidth) - kMargin
                               : clear_button_left() - kChipGap;
    chip_ = {chip_right - chip_width, kClearButtonTop, chip_right, kClearButtonTop + kChipHeight};
    prompt_ = {};
    if (notifications_.empty() && empty_state_for(context_.phone).prompt) {
        const int left = (static_cast<int>(kHistoryViewWidth) - kPromptWidth) / 2;
        prompt_ = {left, kPromptTop, left + kPromptWidth, kPromptTop + kPromptHeight};
    }

    // Everything the picture depends on: layout, text, colours and the counts in the header.
    Fingerprint hash;
    hash.add(chip_label_);
    hash.add(static_cast<std::int64_t>(chip_tone_));
    hash.add(context_.phone.state);
    hash.add(static_cast<std::int64_t>(content_height_));
    hash.add(static_cast<std::int64_t>(hidden_count_));
    hash.add(static_cast<std::int64_t>(notifications_.size()));
    std::int64_t unread = 0;
    for (const auto& notification : notifications_) unread += notification.read ? 0 : 1;
    hash.add(unread);
    for (const Row& row : rows_) {
        hash.add(static_cast<std::int64_t>(row.is_day ? 1 : 0));
        hash.add(static_cast<std::int64_t>(row.top));
        hash.add(static_cast<std::int64_t>(row.height));
        hash.add(row.label);
        if (row.is_day) continue;  // headings have no card data
        hash.add(row.style.name);
        hash.add(row.style.monogram);
        hash.add(static_cast<std::int64_t>(row.style.accent.red) << 16 |
                 static_cast<std::int64_t>(row.style.accent.green) << 8 | row.style.accent.blue);
        hash.add(row.title);
        hash.add(row.time_label);
        for (const auto& line : row.lines) hash.add(line);
        hash.add(static_cast<std::int64_t>((row.read ? 1 : 0) | (row.expandable ? 2 : 0) |
                                           (row.expanded ? 4 : 0)));
    }
    signature_ = hash.value();
}

std::optional<std::pair<int, int>> HistoryView::card_span(std::size_t index) const {
    for (const Row& row : rows_) {
        if (!row.is_day && row.index == index) return std::make_pair(row.top, row.top + row.height);
    }
    return std::nullopt;
}

std::optional<std::string> HistoryView::card_time_label(std::size_t index) const {
    for (const Row& row : rows_) {
        if (!row.is_day && row.index == index) return row.time_label;
    }
    return std::nullopt;
}

std::optional<HistoryHit> HistoryView::hit_test(int x, int y, int scroll_offset) const {
    if (x < 0 || x >= static_cast<int>(kHistoryViewWidth) || y < 0 ||
        y >= static_cast<int>(kHistoryViewHeight)) {
        return std::nullopt;
    }
    const int content_y = y + std::clamp(scroll_offset, 0, maximum_scroll_offset());

    if (content_y < kHeaderHeight) {
        if (chip_.contains(x, content_y, kChipHitPadding)) {
            return HistoryHit{HistoryHitKind::kPhoneChip, 0};
        }
        if (!notifications_.empty() && x >= clear_button_left() - kClearButtonHitPadding &&
            x < clear_button_left() + kClearButtonWidth + kClearButtonHitPadding &&
            content_y >= kClearButtonTop - kClearButtonHitPadding &&
            content_y < kClearButtonTop + kClearButtonHeight + kClearButtonHitPadding) {
            return HistoryHit{HistoryHitKind::kClearAll, 0};
        }
        return std::nullopt;
    }
    if (notifications_.empty()) {
        if (prompt_.contains(x, content_y, kClearButtonHitPadding)) {
            return HistoryHit{HistoryHitKind::kPairPrompt, 0};
        }
        return std::nullopt;
    }

    for (const Row& row : rows_) {
        if (row.is_day || content_y < row.top || content_y >= row.top + row.height) continue;
        if (x < kMargin || x >= static_cast<int>(kHistoryViewWidth) - kMargin) return std::nullopt;
        const Point center = dismiss_center(row.top);
        if (std::abs(static_cast<float>(x) - center.x) <= static_cast<float>(kDismissHitRadius) &&
            std::abs(static_cast<float>(content_y) - center.y) <= static_cast<float>(kDismissHitRadius)) {
            return HistoryHit{HistoryHitKind::kDismiss, row.index};
        }
        return HistoryHit{HistoryHitKind::kCard, row.index};
    }
    return std::nullopt;
}

std::vector<std::uint8_t> HistoryView::render_content() const {
    Canvas canvas(kHistoryViewWidth, static_cast<std::uint32_t>(content_height_), kBackgroundBottom);
    const auto width = static_cast<float>(kHistoryViewWidth);
    const auto height = static_cast<float>(content_height_);
    const Typography& typography = Typography::shared();

    paint_background(canvas, width, height);

    // ---- Header ----
    const float badge_left = static_cast<float>(kMargin);
    const float badge_top = 42.0F;
    canvas.fill_rounded_rect(badge_left, badge_top, badge_left + 64.0F, badge_top + 64.0F, 20.0F,
                             {112, 156, 255, 255}, {96, 72, 246, 255});
    draw_bell(canvas, badge_left + 32.0F, badge_top + 33.0F, 38.0F, kWhite);
    const float title_left = badge_left + 64.0F + 22.0F;
    canvas.draw_text(title_left, 84.0F, "Notifications", kTitleFont, kTextStrong);

    std::size_t unread = 0;
    for (const auto& notification : notifications_) {
        if (!notification.read) ++unread;
    }
    if (notifications_.empty()) {
        canvas.draw_text(title_left, 118.0F, empty_state_for(context_.phone).subtitle, kSubtitleFont,
                         kTextMuted);
    } else {
        float cursor = title_left;
        if (unread != 0U) {
            cursor += canvas.draw_text(cursor, 118.0F, std::to_string(unread) + " new",
                                       {kSubtitleFont.size, FontWeight::kSemiBold, 0.0F}, kAccentBlue);
            cursor += canvas.draw_text(cursor, 118.0F, "  \xC2\xB7  ", kSubtitleFont, kTextFaint);
        }
        canvas.draw_text(cursor, 118.0F, std::to_string(notifications_.size()) + " total",
                         kSubtitleFont, kTextMuted);

        const auto button_left = static_cast<float>(clear_button_left());
        const auto button_top = static_cast<float>(kClearButtonTop);
        const float button_right = button_left + static_cast<float>(kClearButtonWidth);
        const float button_bottom = button_top + static_cast<float>(kClearButtonHeight);
        canvas.fill_rounded_rect(button_left, button_top, button_right, button_bottom,
                                 static_cast<float>(kClearButtonHeight) / 2.0F, kButtonFill);
        canvas.stroke_rounded_rect(button_left, button_top, button_right, button_bottom,
                                   static_cast<float>(kClearButtonHeight) / 2.0F, 1.5F, kButtonBorder);
        canvas.draw_text((button_left + button_right) / 2.0F,
                         button_top + static_cast<float>(kClearButtonHeight) / 2.0F +
                             typography.cap_height(kButtonFont) / 2.0F,
                         "Clear all", kButtonFont, {214, 221, 238, 255}, TextAlign::kCenter);
    }

    // The phone's status: a coloured dot and a short label, which opens the phone screen.
    {
        const Color tone = tone_color(chip_tone_);
        const auto left = static_cast<float>(chip_.left);
        const auto top = static_cast<float>(chip_.top);
        const auto right = static_cast<float>(chip_.right);
        const auto bottom = static_cast<float>(chip_.bottom);
        canvas.fill_rounded_rect(left, top, right, bottom, (bottom - top) / 2.0F,
                                 mix(kButtonFill, tone, chip_tone_ == Tone::kNeutral ? 0.0F : 0.12F));
        canvas.stroke_rounded_rect(left, top, right, bottom, (bottom - top) / 2.0F, 1.5F,
                                   mix(kButtonBorder, tone, chip_tone_ == Tone::kNeutral ? 0.0F : 0.55F));
        canvas.fill_circle(left + 26.0F + 6.0F, (top + bottom) / 2.0F, 6.0F, tone);
        canvas.draw_text(left + 26.0F + 12.0F + 12.0F, (top + bottom) / 2.0F + typography.cap_height(kButtonFont) / 2.0F,
                         chip_label_, kButtonFont, {214, 221, 238, 255});
    }

    // ---- Empty state ----
    if (notifications_.empty()) {
        const float center_x = width / 2.0F;
        canvas.draw_shadow(center_x - 66.0F, 184.0F, center_x + 66.0F, 316.0F, 66.0F, 36.0F, 12.0F,
                           {0, 0, 0, 120});
        canvas.fill_circle(center_x, 250.0F, 66.0F, {33, 41, 62, 255});
        canvas.fill_rounded_rect(center_x - 64.0F, 186.0F, center_x + 64.0F, 314.0F, 64.0F,
                                 {30, 37, 57, 255}, {21, 26, 41, 255});
        canvas.stroke_rounded_rect(center_x - 66.0F, 184.0F, center_x + 66.0F, 316.0F, 66.0F, 1.5F,
                                   {46, 56, 82, 255});
        draw_bell(canvas, center_x, 252.0F, 64.0F, {128, 150, 205, 255});
        const EmptyState empty = empty_state_for(context_.phone);
        canvas.draw_text(center_x, 376.0F, empty.title, kEmptyTitleFont, {233, 238, 249, 255},
                         TextAlign::kCenter);
        float body_baseline = 416.0F;
        for (const auto& line : typography.wrap(empty.body, kEmptyBodyFont, 760.0F, 2)) {
            canvas.draw_text(center_x, body_baseline, line, kEmptyBodyFont, kTextMuted, TextAlign::kCenter);
            body_baseline += 32.0F;
        }
        if (prompt_.right > prompt_.left) {
            const auto left = static_cast<float>(prompt_.left);
            const auto top = static_cast<float>(prompt_.top);
            const auto right = static_cast<float>(prompt_.right);
            const auto bottom = static_cast<float>(prompt_.bottom);
            canvas.draw_shadow(left, top, right, bottom, (bottom - top) / 2.0F, 24.0F, 8.0F,
                               faded(kPrimaryBottom, 0.45F));
            canvas.fill_rounded_rect(left, top, right, bottom, (bottom - top) / 2.0F, kPrimaryTop,
                                     kPrimaryBottom);
            canvas.draw_text((left + right) / 2.0F,
                             (top + bottom) / 2.0F + typography.cap_height(kPairButtonFont) / 2.0F,
                             "Pair an iPhone", kPairButtonFont, kWhite, TextAlign::kCenter);
        }
        return canvas.take_pixels();
    }

    // ---- Rows ----
    const auto card_left = static_cast<float>(kMargin);
    const auto card_right = static_cast<float>(kHistoryViewWidth) - static_cast<float>(kMargin);
    for (const Row& row : rows_) {
        const auto top = static_cast<float>(row.top);
        if (row.is_day) {
            const float end = canvas.draw_text(card_left + 4.0F, top + 40.0F,
                                               uppercase_ascii(row.label), kDayFont, kTextMuted);
            canvas.fill_rect(card_left + 4.0F + end + 20.0F, top + 33.0F, card_right, top + 34.0F, kLine);
            continue;
        }

        const auto bottom = static_cast<float>(row.top + row.height);
        const Color accent = row.style.accent;
        canvas.draw_shadow(card_left, top, card_right, bottom, static_cast<float>(kCardRadius), 30.0F,
                           10.0F, {0, 0, 0, 120});
        canvas.fill_rounded_rect(card_left, top, card_right, bottom, static_cast<float>(kCardRadius),
                                 row.read ? kCardReadTop : kCardUnreadTop,
                                 row.read ? kCardReadBottom : kCardUnreadBottom);
        canvas.stroke_rounded_rect(card_left, top, card_right, bottom, static_cast<float>(kCardRadius),
                                   1.5F,
                                   row.read ? kBorderRead : mix(kBorderUnread, accent, 0.22F));

        // Avatar: the app's colour with its first letter.
        const float avatar_left = card_left + static_cast<float>(kCardPaddingX);
        const float avatar_top = top + static_cast<float>(kAvatarTop);
        const float avatar_dim = row.read ? 0.28F : 0.0F;
        if (!row.read) {
            canvas.draw_shadow(avatar_left, avatar_top, avatar_left + static_cast<float>(kAvatarSize),
                               avatar_top + static_cast<float>(kAvatarSize), 22.0F, 26.0F, 9.0F,
                               faded(accent, 0.34F));
        }
        canvas.fill_rounded_rect(avatar_left, avatar_top, avatar_left + static_cast<float>(kAvatarSize),
                                 avatar_top + static_cast<float>(kAvatarSize), 22.0F,
                                 mix(mix(accent, kWhite, 0.16F), kCardReadTop, avatar_dim),
                                 mix(mix(accent, {0, 0, 0, 255}, 0.28F), kCardReadTop, avatar_dim));
        canvas.draw_text(avatar_left + static_cast<float>(kAvatarSize) / 2.0F,
                         avatar_top + static_cast<float>(kAvatarSize) / 2.0F +
                             typography.cap_height(kMonogramFont) / 2.0F,
                         row.style.monogram, kMonogramFont, kWhite, TextAlign::kCenter);

        // App name, unread dot and time on one line.
        const float text_left = card_left + static_cast<float>(kTextOffset);
        const float text_right = card_right - static_cast<float>(kCardPaddingX + kDismissDiameter + 22);
        const Color app_color = mix(accent, kWhite, row.read ? 0.05F : 0.32F);
        canvas.draw_text(text_left, top + static_cast<float>(kAppBaseline),
                         uppercase_ascii(row.style.name), kAppFont,
                         row.read ? mix(app_color, kCardReadBottom, 0.35F) : app_color);
        const float time_width = typography.measure(row.time_label, kTimeFont);
        canvas.draw_text(text_right, top + static_cast<float>(kAppBaseline), row.time_label,
                         kTimeFont, row.read ? kTextFaint : kTextMuted, TextAlign::kRight);
        if (!row.read) {
            canvas.fill_circle(text_right - time_width - 17.0F, top + static_cast<float>(kAppBaseline) - 6.5F,
                               5.0F, accent);
        }

        canvas.draw_text(text_left, top + static_cast<float>(kTitleBaseline), row.title, kCardTitleFont,
                         row.read ? kTextReadTitle : kTextUnreadTitle);
        for (std::size_t line = 0; line < row.lines.size(); ++line) {
            canvas.draw_text(text_left,
                             top + static_cast<float>(kFirstLineBaseline) +
                                 static_cast<float>(line) * static_cast<float>(kLineHeight),
                             row.lines[line], kMessageFont, row.read ? kTextReadBody : kTextUnreadBody);
        }

        // Round dismiss button.
        const Point center = dismiss_center(row.top);
        canvas.fill_circle(center.x, center.y, static_cast<float>(kDismissDiameter) / 2.0F,
                           {36, 44, 65, 255});
        draw_cross(canvas, center.x, center.y, 8.0F, 3.0F, {156, 168, 196, 255});

        if (row.expandable) {
            draw_chevron(canvas, center.x, bottom - 34.0F, 9.0F, 3.0F, row.expanded, kTextMuted);
        }
    }

    const float footer_baseline = height - static_cast<float>(kFooterHeight) + 52.0F;
    if (hidden_count_ > 0) {
        canvas.draw_text(width / 2.0F, footer_baseline - 14.0F,
                         std::to_string(hidden_count_) + " older notification" +
                             (hidden_count_ == 1 ? "" : "s") + " not shown",
                         kFooterFont, kTextMuted, TextAlign::kCenter);
        canvas.draw_text(width / 2.0F, footer_baseline + 14.0F, "Clear some to see them",
                         kFooterFont, kTextFaint, TextAlign::kCenter);
    } else {
        canvas.draw_text(width / 2.0F, footer_baseline,
                         "Tap a card to expand  \xC2\xB7  Tap \xC3\x97 to clear one", kFooterFont,
                         kTextFaint, TextAlign::kCenter);
    }
    return canvas.take_pixels();
}

std::vector<std::uint8_t> HistoryView::render_viewport(int scroll_offset) const {
    const int clamped_scroll = std::clamp(scroll_offset, 0, maximum_scroll_offset());
    const auto content = render_content();
    const auto row_bytes = static_cast<std::size_t>(kHistoryViewWidth) * kHistoryViewBytesPerPixel;
    std::vector<std::uint8_t> viewport(row_bytes * static_cast<std::size_t>(kHistoryViewHeight));
    const auto first = content.begin() + static_cast<std::ptrdiff_t>(
                                             static_cast<std::size_t>(clamped_scroll) * row_bytes);
    std::copy_n(first, viewport.size(), viewport.begin());
    return viewport;
}

}  // namespace frame_notify::ui
