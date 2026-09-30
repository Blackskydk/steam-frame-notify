#include "system/update_check.h"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <utility>
#include <vector>

namespace frame_notify::system {
namespace {

struct ParsedVersion {
    std::vector<long> numbers;
    bool suffix = false;
};

std::optional<ParsedVersion> parse_version(std::string text) {
    if (!text.empty() && (text.front() == 'v' || text.front() == 'V')) text.erase(text.begin());
    ParsedVersion parsed;
    const std::size_t cut = text.find_first_of("-+");
    if (cut != std::string::npos) {
        parsed.suffix = true;
        text.resize(cut);
    }
    if (text.empty()) return std::nullopt;
    std::size_t start = 0;
    while (start <= text.size()) {
        std::size_t end = text.find('.', start);
        if (end == std::string::npos) end = text.size();
        long value = 0;
        const auto result = std::from_chars(text.data() + start, text.data() + end, value);
        if (end == start || result.ec != std::errc{} || result.ptr != text.data() + end) return std::nullopt;
        parsed.numbers.push_back(value);
        start = end + 1;
    }
    return parsed;
}

std::string curl_failure_message(int exit_code, bool started, const std::string& output) {
    if (!started) return "curl is needed to check for updates, and it could not be run.";
    switch (exit_code) {
    case 6:
    case 7:
        return "Could not reach github.com. Is the Frame online?";
    case 22:
        return "GitHub has no release to tell about.";
    case 28:
        return "github.com did not answer in time.";
    default:
        break;
    }
    std::string message = "The check failed (curl exit code " + std::to_string(exit_code) + ")";
    if (!output.empty() && output.size() < 160) message += ": " + output;
    return message + ".";
}

}  // namespace

UpdateChecker::UpdateChecker(Options options) : options_(std::move(options)) {}

std::string UpdateChecker::tag_from_url(const std::string& url) {
    static const std::string marker = "/releases/tag/";
    const std::size_t at = url.find(marker);
    if (at == std::string::npos) return {};
    std::string tag = url.substr(at + marker.size());
    const std::size_t end = tag.find_first_of("?#/ \t\r\n");
    if (end != std::string::npos) tag.resize(end);
    return tag;
}

std::string UpdateChecker::version_from_tag(const std::string& tag) {
    return !tag.empty() && (tag.front() == 'v' || tag.front() == 'V') ? tag.substr(1) : tag;
}

std::optional<int> UpdateChecker::compare_versions(const std::string& a, const std::string& b) {
    const auto left = parse_version(a);
    const auto right = parse_version(b);
    if (!left || !right) return std::nullopt;
    const std::size_t length = std::max(left->numbers.size(), right->numbers.size());
    for (std::size_t index = 0; index < length; ++index) {
        const long first = index < left->numbers.size() ? left->numbers[index] : 0;
        const long second = index < right->numbers.size() ? right->numbers[index] : 0;
        if (first != second) return first < second ? -1 : 1;
    }
    if (left->suffix != right->suffix) return left->suffix ? -1 : 1;
    return 0;
}

bool UpdateChecker::start() {
    if (status_.state == UpdateState::kChecking) return false;
    status_ = UpdateStatus{};
    status_.state = UpdateState::kChecking;
    // -I asks for the headers only, -L follows the redirect to the newest release's page, and the
    // address that was finally reached is all that is printed.
    const bool started = process_.start({options_.curl, "-fsSIL", "--max-time", "15", "-o", "/dev/null", "-w",
                                         "%{url_effective}", options_.releases_url},
                                        {}, options_.limit);
    if (!started) {
        status_.state = UpdateState::kFailed;
        status_.message = curl_failure_message(-1, false, process_.result().output);
    }
    return true;
}

bool UpdateChecker::poll() {
    if (status_.state != UpdateState::kChecking) return false;
    if (!process_.poll()) return false;

    const ProcessResult& result = process_.result();
    if (result.timed_out) {
        status_.state = UpdateState::kFailed;
        status_.message = "github.com did not answer in time.";
        return true;
    }
    if (!result.started || result.exit_code != 0) {
        status_.state = UpdateState::kFailed;
        status_.message = curl_failure_message(result.exit_code, result.started, result.output);
        return true;
    }
    const std::string tag = tag_from_url(result.output);
    if (tag.empty()) {
        status_.state = UpdateState::kFailed;
        status_.message = "GitHub's answer did not name a release.";
        return true;
    }
    status_.latest = version_from_tag(tag);
    const auto order = compare_versions(options_.current_version, status_.latest);
    if (!order) {
        status_.state = UpdateState::kUnknown;
    } else {
        status_.state = *order < 0 ? UpdateState::kAvailable : UpdateState::kCurrent;
    }
    return true;
}

}  // namespace frame_notify::system
