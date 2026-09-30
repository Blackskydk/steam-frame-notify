#include "history/store.h"

#include "ipc/json_line.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <system_error>
#include <utility>

namespace frame_notify::history {
namespace {

std::int64_t now_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

using ipc::escape_json_string;

std::string serialize(const ipc::NotificationMessage& notification) {
    const std::string app_id = notification.app_id.empty()
                                   ? std::string()
                                   : ",\"app_id\":\"" + escape_json_string(notification.app_id) + "\"";
    return "{\"type\":\"notification\",\"id\":\"" + escape_json_string(notification.id) +
           "\",\"app\":\"" + escape_json_string(notification.app) + "\"" + app_id + ",\"title\":\"" +
           escape_json_string(notification.title) + "\",\"message\":\"" +
           escape_json_string(notification.message) + "\",\"timestamp\":\"" +
           escape_json_string(notification.timestamp) + "\",\"received_at\":\"" +
           std::to_string(notification.received_at) + "\",\"read\":\"" +
           (notification.read ? "1" : "0") + "\",\"dismissed\":\"" +
           (notification.dismissed ? "1" : "0") + "\"}";
}

}  // namespace

bool Store::initialize() {
    const char* state_home = std::getenv("XDG_STATE_HOME");
    if (state_home != nullptr && *state_home != '\0') {
        return initialize_at(std::string(state_home) + "/frame-notify");
    }
    const char* home = std::getenv("HOME");
    if (home == nullptr || *home == '\0') {
        std::cerr << "[History] Neither XDG_STATE_HOME nor HOME is set\n";
        return false;
    }
    return initialize_at(std::string(home) + "/.local/state/frame-notify");
}

bool Store::initialize_at(std::string state_directory) {
    namespace fs = std::filesystem;
    state_directory_ = std::move(state_directory);
    path_ = (fs::path(state_directory_) / "history.jsonl").string();

    std::error_code filesystem_error;
    fs::create_directories(state_directory_, filesystem_error);
    if (filesystem_error) {
        std::cerr << "[History] Could not create " << state_directory_ << ": "
                  << filesystem_error.message() << '\n';
        return false;
    }
    fs::permissions(state_directory_, fs::perms::owner_all, fs::perm_options::replace,
                    filesystem_error);
    if (filesystem_error) {
        std::cerr << "[History] Could not restrict state-directory permissions: "
                  << filesystem_error.message() << '\n';
        return false;
    }

    const bool history_exists = fs::exists(path_, filesystem_error);
    if (filesystem_error) {
        std::cerr << "[History] Could not inspect " << path_ << ": "
                  << filesystem_error.message() << '\n';
        return false;
    }
    if (history_exists) {
        fs::permissions(path_, fs::perms::owner_read | fs::perms::owner_write,
                        fs::perm_options::replace, filesystem_error);
        if (filesystem_error) {
            std::cerr << "[History] Could not restrict history-file permissions: "
                      << filesystem_error.message() << '\n';
            return false;
        }
    }

    std::ifstream input(path_, std::ios::binary);
    bool needs_rewrite = false;
    if (!input && history_exists) {
        std::cerr << "[History] Could not open " << path_ << '\n';
        return false;
    }
    if (input) {
        std::string line;
        while (std::getline(input, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            std::string error;
            auto notification = ipc::parse_notification_json(line, error);
            if (!notification || ids_.find(notification->id) != ids_.end()) {
                std::cerr << "[History] Skipping invalid stored entry: "
                          << (notification ? "duplicate ID" : error) << '\n';
                needs_rewrite = true;
                continue;
            }
            ids_.insert(notification->id);
            notifications_.push_back(std::move(*notification));
        }
        if (!input.eof()) {
            std::cerr << "[History] Could not read " << path_ << '\n';
            return false;
        }
    }

    const auto size_before_prune = notifications_.size();
    prune();
    needs_rewrite = needs_rewrite || notifications_.size() != size_before_prune;
    if (needs_rewrite && !save()) return false;
    std::cout << "[History] Loaded " << notifications_.size() << " notification(s) from " << path_
              << '\n';
    return true;
}

bool Store::add(ipc::NotificationMessage notification) {
    if (notification.id.empty() || ids_.find(notification.id) != ids_.end()) return false;

    if (notification.received_at <= 0) notification.received_at = now_seconds();

    ids_.insert(notification.id);
    notifications_.insert(notifications_.begin(), std::move(notification));
    prune();
    if (!path_.empty() && !save()) {
        std::cerr << "[History] Notification is in memory but could not be persisted\n";
    }
    return true;
}

bool Store::mark_read(std::string_view id) {
    const auto notification = std::find_if(
        notifications_.begin(), notifications_.end(),
        [id](const ipc::NotificationMessage& candidate) {
            return std::string_view(candidate.id) == id;
        });
    if (notification == notifications_.end() || notification->dismissed) return false;
    if (notification->read) return true;

    notification->read = true;
    if (!path_.empty() && !save()) {
        std::cerr << "[History] Read state is in memory but could not be persisted\n";
    }
    return true;
}

std::size_t Store::mark_all_read() {
    std::size_t changed = 0;
    for (auto& notification : notifications_) {
        if (!notification.dismissed && !notification.read) {
            notification.read = true;
            ++changed;
        }
    }
    if (changed != 0U && !path_.empty() && !save()) {
        std::cerr << "[History] Read state is in memory but could not be persisted\n";
    }
    return changed;
}

bool Store::dismiss(std::string_view id) {
    const auto notification = std::find_if(
        notifications_.begin(), notifications_.end(),
        [id](const ipc::NotificationMessage& candidate) {
            return std::string_view(candidate.id) == id;
        });
    if (notification == notifications_.end()) return false;
    if (notification->dismissed) return true;

    notification->read = true;
    notification->dismissed = true;
    if (!path_.empty() && !save()) {
        std::cerr << "[History] Dismissal is in memory but could not be persisted\n";
    }
    return true;
}

std::size_t Store::dismiss_all() {
    std::size_t changed = 0;
    for (auto& notification : notifications_) {
        if (!notification.dismissed) {
            notification.read = true;
            notification.dismissed = true;
            ++changed;
        }
    }
    if (changed != 0U && !path_.empty() && !save()) {
        std::cerr << "[History] Dismissals are in memory but could not be persisted\n";
    }
    return changed;
}

void Store::prune() {
    const auto oldest_allowed = now_seconds() -
                                static_cast<std::int64_t>(maximum_age_days_) * 24 * 60 * 60;
    for (auto notification = notifications_.begin(); notification != notifications_.end();) {
        if (notification->received_at < oldest_allowed) {
            ids_.erase(notification->id);
            notification = notifications_.erase(notification);
        } else {
            ++notification;
        }
    }
    while (notifications_.size() > maximum_size_) {
        ids_.erase(notifications_.back().id);
        notifications_.pop_back();
    }
}

bool Store::save() const {
    namespace fs = std::filesystem;
    const fs::path temporary = fs::path(path_).concat(".tmp");
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) {
        std::cerr << "[History] Could not open temporary history file\n";
        return false;
    }

    std::error_code filesystem_error;
    fs::permissions(temporary, fs::perms::owner_read | fs::perms::owner_write,
                    fs::perm_options::replace, filesystem_error);
    if (filesystem_error) {
        std::cerr << "[History] Could not restrict history-file permissions: "
                  << filesystem_error.message() << '\n';
        output.close();
        fs::remove(temporary, filesystem_error);
        return false;
    }

    for (const auto& notification : notifications_) output << serialize(notification) << '\n';
    output.flush();
    if (!output) {
        std::cerr << "[History] Could not write temporary history file\n";
        output.close();
        fs::remove(temporary, filesystem_error);
        return false;
    }
    output.close();

#ifdef _WIN32
    fs::remove(path_, filesystem_error);
    filesystem_error.clear();
#endif
    fs::rename(temporary, path_, filesystem_error);
    if (filesystem_error) {
        std::cerr << "[History] Could not replace " << path_ << ": "
                  << filesystem_error.message() << '\n';
        fs::remove(temporary, filesystem_error);
        return false;
    }
    return true;
}

}  // namespace frame_notify::history
