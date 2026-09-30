#pragma once

#include "ipc/notification_message.h"

#include <cstddef>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace frame_notify::history {

class Store {
public:
    explicit Store(std::size_t maximum_size = 100, int maximum_age_days = 30)
        : maximum_size_(maximum_size), maximum_age_days_(maximum_age_days) {}

    bool initialize();
    bool initialize_at(std::string state_directory);
    bool add(ipc::NotificationMessage notification);
    bool mark_read(std::string_view id);
    std::size_t mark_all_read();
    bool dismiss(std::string_view id);
    std::size_t dismiss_all();
    [[nodiscard]] const std::vector<ipc::NotificationMessage>& notifications() const noexcept {
        return notifications_;
    }
    [[nodiscard]] const std::string& path() const noexcept { return path_; }

private:
    void prune();
    bool save() const;

    std::size_t maximum_size_;
    int maximum_age_days_;
    std::string state_directory_;
    std::string path_;
    std::vector<ipc::NotificationMessage> notifications_;
    std::unordered_set<std::string> ids_;
};

}  // namespace frame_notify::history
