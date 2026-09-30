#pragma once

#include <cstdint>
#include <string>

namespace frame_notify::ipc {

struct NotificationMessage {
    std::string id;
    std::string app;
    std::string app_id;  // optional bundle identifier; only used to pick the app's colour
    std::string title;
    std::string message;
    std::string timestamp;
    std::int64_t received_at = 0;
    bool read = false;
    bool dismissed = false;
    bool toast = true;
};

}  // namespace frame_notify::ipc
