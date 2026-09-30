#pragma once

#include "ipc/notification_message.h"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace frame_notify::ipc {

// One JSON object whose values are all strings: the only shape Frame Notify's local protocols use.
using JsonFields = std::map<std::string, std::string>;

[[nodiscard]] std::optional<JsonFields> parse_flat_json(std::string_view line, std::string& error);
// {"name":"value",...} for the given fields, in order, without a trailing newline.
[[nodiscard]] std::string serialize_flat_json(
    const std::vector<std::pair<std::string, std::string>>& fields);
[[nodiscard]] std::string escape_json_string(std::string_view value);

[[nodiscard]] std::optional<NotificationMessage> parse_notification_json(
    std::string_view line, std::string& error);

}  // namespace frame_notify::ipc
