#pragma once

#include "ipc/notification_message.h"

#include <memory>
#include <string>
#include <vector>

namespace frame_notify::ipc {

class SocketServer {
public:
    SocketServer();
    ~SocketServer();

    SocketServer(const SocketServer&) = delete;
    SocketServer& operator=(const SocketServer&) = delete;

    bool start();
    [[nodiscard]] std::vector<NotificationMessage> poll();
    [[nodiscard]] const std::string& path() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace frame_notify::ipc
