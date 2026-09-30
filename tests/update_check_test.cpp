#include "system/update_check.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <thread>

// The update check: reading a version off GitHub's redirect, comparing versions, and the states
// the check goes through, against a fake `curl`.

namespace {

using namespace frame_notify::system;
namespace fs = std::filesystem;

int failures = 0;

void expect(bool condition, const char* expression, int line) {
    if (!condition) {
        std::cerr << "update_check_test.cpp:" << line << ": expectation failed: " << expression << '\n';
        ++failures;
    }
}

#define EXPECT(condition) expect((condition), #condition, __LINE__)

std::string read_file(const fs::path& path) {
    std::ifstream stream(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

void write_script(const fs::path& path, const std::string& text) {
    std::ofstream(path, std::ios::binary) << text;
    ::chmod(path.c_str(), 0700);
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

// Polls until the check is over or the time is up.
bool finish(UpdateChecker& checker, std::chrono::milliseconds limit = std::chrono::seconds(8)) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        if (checker.poll()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

constexpr char kReleases[] = "https://github.com/example/frame-notify/releases/latest";

}  // namespace

int main() {
    const fs::path work = fs::temp_directory_path() / ("frame-notify-update-test-" + std::to_string(::getpid()));
    fs::create_directories(work);

    // ---- Reading the release off the address ----
    EXPECT(UpdateChecker::tag_from_url("https://github.com/o/r/releases/tag/v1.2.3") == "v1.2.3");
    EXPECT(UpdateChecker::tag_from_url("https://github.com/o/r/releases/tag/v1.2.3?expanded=true") == "v1.2.3");
    EXPECT(UpdateChecker::tag_from_url("https://github.com/o/r/releases/tag/v1.2.3/") == "v1.2.3");
    EXPECT(UpdateChecker::tag_from_url("https://github.com/o/r/releases/tag/1.0.0-rc1\n") == "1.0.0-rc1");
    EXPECT(UpdateChecker::tag_from_url("https://github.com/o/r/releases").empty());
    EXPECT(UpdateChecker::tag_from_url("https://github.com/o/r/releases/latest").empty());
    EXPECT(UpdateChecker::tag_from_url("").empty());
    EXPECT(UpdateChecker::version_from_tag("v0.1.1") == "0.1.1" && UpdateChecker::version_from_tag("0.1.1") == "0.1.1");
    EXPECT(UpdateChecker::version_from_tag("V2") == "2" && UpdateChecker::version_from_tag("").empty());

    // ---- Comparing versions ----
    const auto order = [](const char* a, const char* b) { return UpdateChecker::compare_versions(a, b); };
    EXPECT(order("0.1.0", "0.1.1") == -1 && order("0.1.1", "0.1.0") == 1 && order("0.1.1", "0.1.1") == 0);
    EXPECT(order("v0.1.1", "0.1.1") == 0 && order("0.1.1", "v0.1.1") == 0);
    EXPECT(order("0.9.0", "0.10.0") == -1);                               // numbers, not text
    EXPECT(order("0.1.9", "0.2.0") == -1 && order("1.0.0", "0.99.99") == 1);
    EXPECT(order("1.0", "1.0.0") == 0 && order("1", "1.0.1") == -1);      // missing parts are zero
    EXPECT(order("0.2.0-rc1", "0.2.0") == -1 && order("0.2.0", "0.2.0-rc1") == 1);   // a pre-release is older
    EXPECT(order("0.0.0-abc1234", "0.1.0") == -1);
    EXPECT(order("0.2.0-rc1", "0.2.0-rc2") == 0);                         // suffixes are not ranked among themselves
    EXPECT(order("0.3.0-rc1", "0.2.0") == 1);
    for (const char* bad : {"", "development", "v", "1..2", "1.2.", ".1", "1.x", "abc", "-1", "1.2.3.four"}) {
        EXPECT(!order(bad, "0.1.0").has_value() && !order("0.1.0", bad).has_value());
    }

    // ---- The check, against a fake curl ----
    const fs::path bin = work / "bin";
    fs::create_directories(bin);
    const fs::path log = work / "curl-arguments.log";
    const auto fake_curl = [&](const std::string& body) {
        write_script(bin / "curl", "#!/bin/sh\necho \"$@\" > '" + log.string() + "'\n" + body + "\n");
    };
    const auto make_checker = [&](const std::string& current) {
        UpdateChecker::Options options;
        options.current_version = current;
        options.releases_url = kReleases;
        options.curl = (bin / "curl").string();
        return UpdateChecker(options);
    };

    {   // A newer release.
        fake_curl("printf 'https://github.com/example/frame-notify/releases/tag/v0.2.0'");
        UpdateChecker checker = make_checker("0.1.1");
        EXPECT(checker.status().state == UpdateState::kIdle && !checker.poll());
        EXPECT(checker.start());
        EXPECT(checker.status().state == UpdateState::kChecking || checker.status().state == UpdateState::kAvailable);
        EXPECT(finish(checker));
        EXPECT(checker.status().state == UpdateState::kAvailable && checker.status().latest == "0.2.0");
        EXPECT(checker.status().message.empty());
        EXPECT(!checker.poll());                                          // nothing more to report
        const std::string arguments = read_file(log);
        EXPECT(contains(arguments, "-fsSIL") && contains(arguments, "--max-time") &&
               contains(arguments, "%{url_effective}") && contains(arguments, kReleases));
        EXPECT(contains(arguments, "-o /dev/null"));
    }
    {   // The same release, or an older one: up to date.
        fake_curl("printf 'https://github.com/example/frame-notify/releases/tag/v0.1.1'");
        UpdateChecker same = make_checker("0.1.1");
        EXPECT(same.start() && finish(same));
        EXPECT(same.status().state == UpdateState::kCurrent && same.status().latest == "0.1.1");
        UpdateChecker ahead = make_checker("0.3.0");
        EXPECT(ahead.start() && finish(ahead));
        EXPECT(ahead.status().state == UpdateState::kCurrent);
    }
    {   // A build with no version of its own cannot be compared.
        fake_curl("printf 'https://github.com/example/frame-notify/releases/tag/v0.1.1'");
        UpdateChecker development = make_checker("development");
        EXPECT(development.start() && finish(development));
        EXPECT(development.status().state == UpdateState::kUnknown && development.status().latest == "0.1.1");
    }
    {   // A release whose tag is not a version: known, not comparable.
        fake_curl("printf 'https://github.com/example/frame-notify/releases/tag/nightly'");
        UpdateChecker nightly = make_checker("0.1.1");
        EXPECT(nightly.start() && finish(nightly));
        EXPECT(nightly.status().state == UpdateState::kUnknown && nightly.status().latest == "nightly");
    }
    {   // Failures say what went wrong.
        struct Case { const char* body; const char* words; };
        for (const Case& failure : {Case{"exit 6", "Could not reach github.com"}, Case{"exit 7", "Could not reach github.com"},
                                    Case{"exit 22", "no release"}, Case{"exit 28", "did not answer"},
                                    Case{"echo boom >&2; exit 9", "exit code 9"}}) {
            fake_curl(failure.body);
            UpdateChecker checker = make_checker("0.1.1");
            EXPECT(checker.start() && finish(checker));
            EXPECT(checker.status().state == UpdateState::kFailed);
            EXPECT(contains(checker.status().message, failure.words));
            EXPECT(checker.status().latest.empty());
        }
        fake_curl("echo boom >&2; exit 9");
        UpdateChecker detailed = make_checker("0.1.1");
        EXPECT(detailed.start() && finish(detailed) && contains(detailed.status().message, "boom"));

        fake_curl("printf 'https://github.com/example/frame-notify/releases'");   // no redirect to a release
        UpdateChecker no_release = make_checker("0.1.1");
        EXPECT(no_release.start() && finish(no_release));
        EXPECT(no_release.status().state == UpdateState::kFailed && contains(no_release.status().message, "did not name a release"));

        fake_curl("printf ''");                                                    // an empty answer
        UpdateChecker empty = make_checker("0.1.1");
        EXPECT(empty.start() && finish(empty) && empty.status().state == UpdateState::kFailed);
    }
    {   // No curl at all.
        UpdateChecker::Options options;
        options.current_version = "0.1.1";
        options.releases_url = kReleases;
        options.curl = (work / "no-such-curl").string();
        UpdateChecker checker(options);
        EXPECT(checker.start());
        if (checker.status().state == UpdateState::kChecking) EXPECT(finish(checker));   // a spawn that fails later
        EXPECT(checker.status().state == UpdateState::kFailed);
        EXPECT(contains(checker.status().message, "curl"));
        EXPECT(checker.start());                                                          // and it can be asked again
        if (checker.status().state == UpdateState::kChecking) EXPECT(finish(checker));
        EXPECT(checker.status().state == UpdateState::kFailed);
    }
    {   // It waits without blocking, and refuses to start twice.
        fake_curl("sleep 1; printf 'https://github.com/example/frame-notify/releases/tag/v0.9.0'");
        UpdateChecker checker = make_checker("0.1.1");
        EXPECT(checker.start());
        EXPECT(checker.status().state == UpdateState::kChecking);
        const auto began = std::chrono::steady_clock::now();
        EXPECT(!checker.poll());
        EXPECT(std::chrono::steady_clock::now() - began < std::chrono::milliseconds(200));   // polling returned at once
        EXPECT(!checker.start());                                         // still waiting: nothing changes
        EXPECT(checker.status().state == UpdateState::kChecking);
        EXPECT(finish(checker) && checker.status().state == UpdateState::kAvailable && checker.status().latest == "0.9.0");
        // Asking again is fine once it is over, and forgets the last answer while it waits.
        fake_curl("sleep 1; exit 6");
        EXPECT(checker.start());
        EXPECT(checker.status().state == UpdateState::kChecking && checker.status().latest.empty());
        EXPECT(finish(checker) && checker.status().state == UpdateState::kFailed);
    }
    {   // An answer that never comes is given up on.
        fake_curl("exec sleep 30");
        UpdateChecker::Options options;
        options.current_version = "0.1.1";
        options.releases_url = kReleases;
        options.curl = (bin / "curl").string();
        options.limit = std::chrono::milliseconds(300);
        UpdateChecker checker(options);
        EXPECT(checker.start());
        const auto began = std::chrono::steady_clock::now();
        EXPECT(finish(checker));
        EXPECT(checker.status().state == UpdateState::kFailed && contains(checker.status().message, "did not answer"));
        EXPECT(std::chrono::steady_clock::now() - began < std::chrono::seconds(5));
    }
    {   // Leaving while a check is running stops it.
        fake_curl("exec sleep 30");
        {
            UpdateChecker checker = make_checker("0.1.1");
            EXPECT(checker.start());
        }
        EXPECT(true);   // no hang and no zombie: reaching here is the test
    }

    std::error_code ignored;
    fs::remove_all(work, ignored);

    if (failures != 0) {
        std::cerr << failures << " expectation(s) failed\n";
        return 1;
    }
    return 0;
}
