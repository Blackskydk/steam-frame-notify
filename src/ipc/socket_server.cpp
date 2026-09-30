#include "ipc/socket_server.h"

#include "ipc/json_line.h"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

#ifdef __linux__
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

namespace frame_notify::ipc {

struct SocketServer::Impl {
    std::string path;
#ifdef __linux__
    struct Client {
        int descriptor = -1;
        std::string buffer;
    };

    int listener = -1;
    bool owns_path = false;
    std::vector<Client> clients;
#endif
};

SocketServer::SocketServer() : impl_(std::make_unique<Impl>()) {}

SocketServer::~SocketServer() {
#ifdef __linux__
    for (const auto& client : impl_->clients) {
        if (client.descriptor >= 0) close(client.descriptor);
    }
    if (impl_->listener >= 0) close(impl_->listener);
    if (impl_->owns_path && !impl_->path.empty()) unlink(impl_->path.c_str());
#endif
}

namespace {

#ifdef __linux__
constexpr std::size_t kMaximumLineBytes = 64U * 1024U;
constexpr std::size_t kMaximumClients = 16;

bool make_nonblocking(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFL, 0);
    return flags >= 0 && fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) == 0;
}

void make_close_on_exec(int descriptor) {
    const int flags = fcntl(descriptor, F_GETFD, 0);
    if (flags >= 0) fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC);
}

bool live_server_exists(const sockaddr_un& address, socklen_t address_size) {
    const int probe = socket(AF_UNIX, SOCK_STREAM, 0);
    if (probe < 0) return true;
    make_close_on_exec(probe);
    const bool connected = connect(probe, reinterpret_cast<const sockaddr*>(&address), address_size) == 0;
    close(probe);
    return connected;
}
#endif

}  // namespace

bool SocketServer::start() {
#ifndef __linux__
    std::cerr << "[IPC] Unix domain sockets are supported only by the Linux/Steam Frame build\n";
    return false;
#else
    if (impl_->listener >= 0) return true;
    const char* runtime_directory = std::getenv("XDG_RUNTIME_DIR");
    if (runtime_directory == nullptr || *runtime_directory == '\0') {
        std::cerr << "[IPC] XDG_RUNTIME_DIR is not set; refusing to create the notification socket\n";
        return false;
    }
    impl_->path = std::string(runtime_directory) + "/frame-notify.sock";

    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (impl_->path.size() >= sizeof(address.sun_path)) {
        std::cerr << "[IPC] Socket path is too long: " << impl_->path << '\n';
        return false;
    }
    std::memcpy(address.sun_path, impl_->path.c_str(), impl_->path.size() + 1U);
    const auto address_size = static_cast<socklen_t>(
        offsetof(sockaddr_un, sun_path) + impl_->path.size() + 1U);

    impl_->listener = socket(AF_UNIX, SOCK_STREAM, 0);
    if (impl_->listener < 0 || !make_nonblocking(impl_->listener)) {
        std::cerr << "[IPC] Could not create nonblocking socket: " << std::strerror(errno) << '\n';
        return false;
    }
    make_close_on_exec(impl_->listener);

    if (bind(impl_->listener, reinterpret_cast<const sockaddr*>(&address), address_size) != 0) {
        if (errno != EADDRINUSE || live_server_exists(address, address_size)) {
            std::cerr << "[IPC] Could not bind " << impl_->path << ": " << std::strerror(errno)
                      << '\n';
            return false;
        }
        if (unlink(impl_->path.c_str()) != 0 ||
            bind(impl_->listener, reinterpret_cast<const sockaddr*>(&address), address_size) != 0) {
            std::cerr << "[IPC] Could not replace stale socket " << impl_->path << ": "
                      << std::strerror(errno) << '\n';
            return false;
        }
    }
    impl_->owns_path = true;
    if (chmod(impl_->path.c_str(), S_IRUSR | S_IWUSR) != 0) {
        std::cerr << "[IPC] Could not restrict socket permissions: " << std::strerror(errno) << '\n';
        return false;
    }
    if (listen(impl_->listener, static_cast<int>(kMaximumClients)) != 0) {
        std::cerr << "[IPC] Could not listen on " << impl_->path << ": " << std::strerror(errno)
                  << '\n';
        return false;
    }
    std::cout << "[IPC] Listening on " << impl_->path << '\n';
    return true;
#endif
}

std::vector<NotificationMessage> SocketServer::poll() {
    std::vector<NotificationMessage> notifications;
#ifdef __linux__
    if (impl_->listener < 0) return notifications;

    while (impl_->clients.size() < kMaximumClients) {
        const int client = accept(impl_->listener, nullptr, nullptr);
        if (client < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                std::cerr << "[IPC] accept failed: " << std::strerror(errno) << '\n';
            }
            break;
        }
        if (!make_nonblocking(client)) {
            close(client);
            continue;
        }
        make_close_on_exec(client);
        impl_->clients.push_back({client, {}});
    }

    for (auto client = impl_->clients.begin(); client != impl_->clients.end();) {
        bool closed = false;
        char chunk[4096];
        while (true) {
            const ssize_t received = recv(client->descriptor, chunk, sizeof(chunk), 0);
            if (received > 0) {
                client->buffer.append(chunk, static_cast<std::size_t>(received));
                if (client->buffer.size() > kMaximumLineBytes) {
                    std::cerr << "[IPC] Closing client after oversized JSON line\n";
                    closed = true;
                    break;
                }
            } else if (received == 0) {
                closed = true;
                break;
            } else {
                if (errno != EAGAIN && errno != EWOULDBLOCK) closed = true;
                break;
            }
        }

        std::size_t newline = 0;
        while ((newline = client->buffer.find('\n')) != std::string::npos) {
            std::string line = client->buffer.substr(0, newline);
            client->buffer.erase(0, newline + 1U);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::string error;
            auto parsed = parse_notification_json(line, error);
            if (parsed) notifications.push_back(std::move(*parsed));
            else std::cerr << "[IPC] Rejected JSON event: " << error << '\n';
        }

        if (closed) {
            close(client->descriptor);
            client = impl_->clients.erase(client);
        } else {
            ++client;
        }
    }
#endif
    return notifications;
}

const std::string& SocketServer::path() const noexcept {
    return impl_->path;
}

}  // namespace frame_notify::ipc
