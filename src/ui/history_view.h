#pragma once

#include "ui/app_style.h"
#include "ui/phone_info.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace frame_notify::ui {

inline constexpr std::uint32_t kHistoryViewWidth = 1280;
inline constexpr std::uint32_t kHistoryViewHeight = 800;
inline constexpr std::uint32_t kHistoryViewBytesPerPixel = 4;

struct HistoryNotification {
    std::string id;
    std::string app;      // display name, or a bundle identifier from older senders
    std::string title;
    std::string message;
    std::string time;     // sender-supplied timestamp; unused when received_at is known
    bool read = false;
    std::int64_t received_at = 0;  // seconds since the epoch; 0 means "just now"
    std::string app_id;   // bundle identifier when the sender knows it; only used for the colour
};

struct HistoryContext {
    std::int64_t now = 0;            // seconds since the epoch
    int utc_offset_seconds = 0;
    std::string expanded_id;         // the card that shows its whole message
    PhoneInfo phone;                 // what the Bluetooth helper says, for the header's status
};

enum class HistoryHitKind {
    kCard,      // the card body: expands or collapses a long message
    kDismiss,   // the round button that clears one notification
    kClearAll,  // the header button
    kPhoneChip,   // the phone status in the header: opens the phone screen
    kPairPrompt,  // the "Pair an iPhone" button shown while nothing is paired
};

struct HistoryHit {
    HistoryHitKind kind;
    std::size_t notification_index;  // unused for kClearAll
};

// Lays out and paints the notification panel: a header, notifications grouped by day, and a
// footer, in one tall RGBA image that the dashboard scrolls by cropping. The image is never taller
// than 8192 pixels, the size every GPU can be expected to accept; older notifications beyond that
// are left out (and counted in hidden_count()) until newer ones are cleared.
class HistoryView {
public:
    void set(std::vector<HistoryNotification> notifications, HistoryContext context);

    [[nodiscard]] const std::vector<HistoryNotification>& notifications() const noexcept {
        return notifications_;
    }
    [[nodiscard]] int content_height() const noexcept { return content_height_; }
    // Notifications too old to fit in the largest texture the panel produces (see set()).
    [[nodiscard]] int hidden_count() const noexcept { return hidden_count_; }
    [[nodiscard]] int maximum_scroll_offset() const noexcept;
    [[nodiscard]] std::optional<HistoryHit> hit_test(int x, int y, int scroll_offset) const;
    // Vertical extent of a notification's card in content coordinates.
    [[nodiscard]] std::optional<std::pair<int, int>> card_span(std::size_t index) const;
    // The time text drawn on a notification's card, e.g. "3 min ago" or "07:41".
    [[nodiscard]] std::optional<std::string> card_time_label(std::size_t index) const;
    // A hash of everything that changes the picture. Equal signatures mean identical pixels, so a
    // re-render that did not change it need not be uploaded again.
    [[nodiscard]] std::uint64_t signature() const noexcept { return signature_; }

    [[nodiscard]] std::vector<std::uint8_t> render_content() const;
    [[nodiscard]] std::vector<std::uint8_t> render_viewport(int scroll_offset) const;

private:
    struct Row {
        bool is_day = false;
        int top = 0;
        int height = 0;
        std::size_t index = 0;          // notification index for cards
        std::string label;              // day heading
        AppStyle style;
        std::string title;
        std::string time_label;
        std::vector<std::string> lines; // wrapped message
        bool read = false;
        bool expandable = false;
        bool expanded = false;
    };

    std::vector<HistoryNotification> notifications_;
    HistoryContext context_;
    struct Area {
        int left = 0;
        int top = 0;
        int right = 0;
        int bottom = 0;
        [[nodiscard]] bool contains(int x, int y, int padding) const noexcept {
            return right > left && x >= left - padding && x < right + padding && y >= top - padding &&
                   y < bottom + padding;
        }
    };

    std::vector<Row> rows_;
    Area chip_;                      // the phone status in the header
    Area prompt_;                    // "Pair an iPhone" in the empty state, if shown
    std::string chip_label_;
    Tone chip_tone_ = Tone::kNeutral;
    int content_height_ = static_cast<int>(kHistoryViewHeight);
    int hidden_count_ = 0;
    std::uint64_t signature_ = 0;
};

}  // namespace frame_notify::ui
