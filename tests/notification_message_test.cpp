#include "history/store.h"
#include "ipc/json_line.h"

#include <chrono>
#include <filesystem>
#include <iostream>
#include <string>

int main() {
    std::string error;
    auto parsed = frame_notify::ipc::parse_notification_json(
        R"({"type":"notification","id":"test-001","app":"Messages","title":"Jane Doe","message":"Are you coming home soon?","timestamp":"2026-09-29T17:02:00+02:00"})",
        error);
    if (!parsed) {
        std::cerr << "Valid notification was rejected: " << error << '\n';
        return 1;
    }
    if (parsed->id != "test-001" || parsed->app != "Messages") {
        std::cerr << "Parsed notification fields are incorrect\n";
        return 1;
    }

    auto unicode = frame_notify::ipc::parse_notification_json(
        R"({"type":"notification","id":"unicode","app":"Nachrichten","title":"Z\u00fcrich","message":"Caf\u00e9 \ud83d\udcf1","timestamp":"now"})",
        error);
    if (!unicode || unicode->title != "Z\xC3\xBC" "rich" ||
        unicode->message != "Caf\xC3\xA9 \xF0\x9F\x93\xB1") {
        std::cerr << "Unicode escape decoding failed: " << error << '\n';
        return 1;
    }

    auto persisted = frame_notify::ipc::parse_notification_json(
        R"({"type":"notification","id":"persisted","app":"Mail","title":"Stored","message":"Restored","timestamp":"now","received_at":"123456","read":"1","dismissed":"1"})",
        error);
    if (!persisted || persisted->received_at != 123456 || !persisted->read ||
        !persisted->dismissed) {
        std::cerr << "Persisted fields were not restored: " << error << '\n';
        return 1;
    }
    auto silent = frame_notify::ipc::parse_notification_json(
        R"({"type":"notification","id":"silent","app":"Messages","title":"Quiet","message":"No toast","timestamp":"now","toast":"false"})",
        error);
    if (!silent || silent->toast || !parsed->toast) {
        std::cerr << "Optional toast flag was parsed incorrectly: " << error << '\n';
        return 1;
    }

    auto with_app_id = frame_notify::ipc::parse_notification_json(
        R"({"type":"notification","id":"typed","app":"Nachrichten","app_id":"com.apple.MobileSMS","title":"Hallo","message":"Bis bald","timestamp":"now"})",
        error);
    if (!with_app_id || with_app_id->app != "Nachrichten" ||
        with_app_id->app_id != "com.apple.MobileSMS" || !parsed->app_id.empty()) {
        std::cerr << "Optional app_id field was parsed incorrectly: " << error << '\n';
        return 1;
    }

    frame_notify::history::Store store(2);
    if (!store.add(*parsed) || store.add(*parsed)) {
        std::cerr << "Notification ID deduplication failed\n";
        return 1;
    }
    unicode->id = "test-002";
    if (!store.add(*unicode) || store.notifications().front().id != "test-002") {
        std::cerr << "Newest-first history insertion failed\n";
        return 1;
    }
    if (!store.mark_read("test-002") || !store.notifications().front().read ||
        !store.dismiss("test-001") || !store.notifications().back().dismissed ||
        store.mark_read("missing") || store.dismiss("missing")) {
        std::cerr << "Notification lifecycle updates failed\n";
        return 1;
    }
    auto expired = *unicode;
    expired.id = "expired";
    expired.received_at = 1;
    if (!store.add(expired) || store.notifications().front().id == "expired") {
        std::cerr << "Age-based retention failed\n";
        return 1;
    }

    auto invalid = frame_notify::ipc::parse_notification_json(
        R"({"type":"notification","id":"missing-fields"})", error);
    if (invalid || error.empty()) {
        std::cerr << "Invalid notification was accepted\n";
        return 1;
    }

    // The flat one-line objects the Bluetooth helper and the overlay exchange.
    {
        using frame_notify::ipc::escape_json_string;
        using frame_notify::ipc::parse_flat_json;
        using frame_notify::ipc::serialize_flat_json;
        std::string flat_error;
        const auto fields = parse_flat_json(
            R"( {"event":"state", "state":"pair_confirm","code":"628640","phone":"Z\u00fcrich \ud83d\udcf1","note":"a\"b\\c\nd"} )",
            flat_error);
        if (!fields || fields->size() != 5U || fields->at("event") != "state" ||
            fields->at("code") != "628640" || fields->at("phone") != "Z\xC3\xBC" "rich \xF0\x9F\x93\xB1" ||
            fields->at("note") != "a\"b\\c\nd") {
            std::cerr << "Flat JSON was not parsed correctly: " << flat_error << '\n';
            return 1;
        }
        const auto empty_object = parse_flat_json("{}", flat_error);
        const auto empty_value = parse_flat_json(R"({"a":""})", flat_error);
        const auto duplicate = parse_flat_json(R"({"a":"1","a":"2"})", flat_error);
        if (!empty_object || !empty_object->empty() || !empty_value || empty_value->at("a") != "" ||
            !duplicate || duplicate->at("a") != "2") {
            std::cerr << "Flat JSON edge cases were parsed incorrectly\n";
            return 1;
        }
        for (const char* bad : {"", "   ", "nonsense", "[]", "{", "}", "{\"a\"}", "{\"a\":}", "{\"a\":1}",
                                "{\"a\":true}", "{\"a\":null}", "{\"a\":\"b\"", "{\"a\":\"b\",}",
                                "{\"a\":\"b\"} trailing", "{\"a\":\"b\"}{\"c\":\"d\"}", "{\"a\":\"b\" \"c\":\"d\"}",
                                "{a:\"b\"}", "{\"a\":{\"b\":\"c\"}}", "{\"a\":[\"b\"]}", "{\"a\":\"\\q\"}",
                                "{\"a\":\"\\u12\"}", "{\"a\":\"\\ud83d\"}", "{\"a\":\"\\ude00\"}",
                                "{\"a\":\"\\ud83d\\u0041\"}", "{\"a\":\"line\nbreak\"}", "{\"a\":\"unterminated\\"}) {
            if (parse_flat_json(bad, flat_error) || flat_error.empty()) {
                std::cerr << "Malformed flat JSON was accepted: " << bad << '\n';
                return 1;
            }
        }
        if (escape_json_string("plain") != "plain" ||
            escape_json_string("q\"b\\n\n\t\r\b\f") != "q\\\"b\\\\n\\n\\t\\r\\b\\f" ||
            escape_json_string(std::string("nul\0x\x1f", 6)) != "nul\\u0000x\\u001f" ||
            escape_json_string("K\xC3\xB8" "benhavn \xF0\x9F\x91\x8D") != "K\xC3\xB8" "benhavn \xF0\x9F\x91\x8D") {
            std::cerr << "JSON string escaping is wrong\n";
            return 1;
        }
        if (serialize_flat_json({}) != "{}" ||
            serialize_flat_json({{"command", "pair"}}) != R"({"command":"pair"})" ||
            serialize_flat_json({{"b", "2"}, {"a", "1"}}) != R"({"b":"2","a":"1"})" ||
            serialize_flat_json({{"k\"ey", "va\nlue"}}) != R"({"k\"ey":"va\nlue"})") {
            std::cerr << "Flat JSON was not serialized correctly\n";
            return 1;
        }
        // Whatever goes out can be read back, including control characters and non-ASCII text.
        const std::vector<std::pair<std::string, std::string>> awkward = {
            {"text", std::string("tab\t quote\" slash\\ nul\0 bell\a ", 30) + "K\xC3\xB8" "benhavn \xF0\x9F\x91\x8D"},
            {"empty", ""},
            {"key with \"quotes\"", "value"}};
        const auto round_trip = parse_flat_json(serialize_flat_json(awkward), flat_error);
        if (!round_trip || round_trip->size() != 3U || round_trip->at("text") != awkward[0].second ||
            round_trip->at("empty") != "" || round_trip->at("key with \"quotes\"") != "value") {
            std::cerr << "Flat JSON did not survive a round trip: " << flat_error << '\n';
            return 1;
        }
        // A notification is still a flat object that parse_flat_json reads.
        if (!parse_flat_json(R"({"type":"notification","id":"x"})", flat_error) ||
            !parse_flat_json(R"({"type":"notification","id":"x"})", flat_error)->count("type")) {
            std::cerr << "Notification objects are no longer flat JSON\n";
            return 1;
        }
    }

    namespace fs = std::filesystem;
    const auto unique = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path state_directory =
        fs::temp_directory_path() / ("frame-notify-history-test-" + std::to_string(unique));
    {
        frame_notify::history::Store persisted_store(3, 30);
        if (!persisted_store.initialize_at(state_directory.string()) ||
            !persisted_store.add(*parsed) || !persisted_store.add(*unicode) ||
            !persisted_store.add(*with_app_id)) {
            std::cerr << "Could not write persistent history\n";
            fs::remove_all(state_directory);
            return 1;
        }
    }
    {
        frame_notify::history::Store restored_store(3, 30);
        if (!restored_store.initialize_at(state_directory.string()) ||
            restored_store.notifications().size() != 3U ||
            restored_store.notifications().front().id != "typed" ||
            restored_store.notifications().front().app_id != "com.apple.MobileSMS" ||
            !restored_store.notifications().back().app_id.empty() ||
            restored_store.mark_all_read() != 3U || !restored_store.dismiss("test-001")) {
            std::cerr << "Could not restore persistent history\n";
            fs::remove_all(state_directory);
            return 1;
        }
    }
    {
        frame_notify::history::Store restored_store(3, 30);
        if (!restored_store.initialize_at(state_directory.string()) ||
            !restored_store.notifications().front().read ||
            !restored_store.notifications().back().dismissed) {
            std::cerr << "Notification lifecycle state was not persisted\n";
            fs::remove_all(state_directory);
            return 1;
        }
        // "Clear all" dismisses everything still visible, once, and persists it.
        if (restored_store.dismiss_all() != 2U || restored_store.dismiss_all() != 0U) {
            std::cerr << "dismiss_all did not clear the remaining notifications\n";
            fs::remove_all(state_directory);
            return 1;
        }
    }
    {
        frame_notify::history::Store restored_store(3, 30);
        if (!restored_store.initialize_at(state_directory.string())) {
            std::cerr << "Could not reload cleared history\n";
            fs::remove_all(state_directory);
            return 1;
        }
        for (const auto& notification : restored_store.notifications()) {
            if (!notification.dismissed || !notification.read) {
                std::cerr << "dismiss_all was not persisted\n";
                fs::remove_all(state_directory);
                return 1;
            }
        }
    }
    fs::remove_all(state_directory);
    return 0;
}
