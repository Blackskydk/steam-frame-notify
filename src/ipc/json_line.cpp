#include "ipc/json_line.h"

#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <map>
#include <string>
#include <utility>

namespace frame_notify::ipc {
namespace {

void append_utf8(std::string& output, std::uint32_t code_point) {
    if (code_point <= 0x7FU) {
        output.push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FFU) {
        output.push_back(static_cast<char>(0xC0U | (code_point >> 6U)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else if (code_point <= 0xFFFFU) {
        output.push_back(static_cast<char>(0xE0U | (code_point >> 12U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    } else {
        output.push_back(static_cast<char>(0xF0U | (code_point >> 18U)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 12U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | ((code_point >> 6U) & 0x3FU)));
        output.push_back(static_cast<char>(0x80U | (code_point & 0x3FU)));
    }
}

class JsonCursor {
public:
    explicit JsonCursor(std::string_view input) : input_(input) {}

    bool parse(std::map<std::string, std::string>& fields, std::string& error) {
        skip_whitespace();
        if (!consume('{')) return fail(error, "expected '{'");
        skip_whitespace();
        if (consume('}')) return finish(error);

        while (position_ < input_.size()) {
            std::string key;
            std::string value;
            if (!parse_string(key, error)) return false;
            skip_whitespace();
            if (!consume(':')) return fail(error, "expected ':' after field name");
            skip_whitespace();
            if (!parse_string(value, error)) {
                if (error.empty()) error = "all field values must be JSON strings";
                return false;
            }
            fields.insert_or_assign(std::move(key), std::move(value));
            skip_whitespace();
            if (consume('}')) return finish(error);
            if (!consume(',')) return fail(error, "expected ',' or '}'");
            skip_whitespace();
        }
        return fail(error, "unterminated object");
    }

private:
    bool parse_string(std::string& output, std::string& error) {
        if (!consume('"')) return fail(error, "expected JSON string");
        while (position_ < input_.size()) {
            const unsigned char character = static_cast<unsigned char>(input_[position_++]);
            if (character == '"') return true;
            if (character < 0x20U) return fail(error, "control character in string");
            if (character != '\\') {
                output.push_back(static_cast<char>(character));
                continue;
            }
            if (position_ >= input_.size()) return fail(error, "unfinished escape sequence");
            const char escape = input_[position_++];
            switch (escape) {
            case '"': output.push_back('"'); break;
            case '\\': output.push_back('\\'); break;
            case '/': output.push_back('/'); break;
            case 'b': output.push_back('\b'); break;
            case 'f': output.push_back('\f'); break;
            case 'n': output.push_back('\n'); break;
            case 'r': output.push_back('\r'); break;
            case 't': output.push_back('\t'); break;
            case 'u': {
                std::uint32_t code_point = 0;
                if (!parse_hex_quad(code_point)) return fail(error, "invalid Unicode escape");
                if (code_point >= 0xD800U && code_point <= 0xDBFFU) {
                    if (position_ + 6U > input_.size() || input_[position_] != '\\' ||
                        input_[position_ + 1U] != 'u') {
                        return fail(error, "missing low surrogate");
                    }
                    position_ += 2U;
                    std::uint32_t low = 0;
                    if (!parse_hex_quad(low) || low < 0xDC00U || low > 0xDFFFU) {
                        return fail(error, "invalid low surrogate");
                    }
                    code_point = 0x10000U + ((code_point - 0xD800U) << 10U) + (low - 0xDC00U);
                } else if (code_point >= 0xDC00U && code_point <= 0xDFFFU) {
                    return fail(error, "unexpected low surrogate");
                }
                append_utf8(output, code_point);
                break;
            }
            default: return fail(error, "unsupported escape sequence");
            }
        }
        return fail(error, "unterminated string");
    }

    bool parse_hex_quad(std::uint32_t& value) {
        if (position_ + 4U > input_.size()) return false;
        value = 0;
        for (int index = 0; index < 4; ++index) {
            const unsigned char character = static_cast<unsigned char>(input_[position_++]);
            value <<= 4U;
            if (character >= '0' && character <= '9') value |= character - '0';
            else if (character >= 'a' && character <= 'f') value |= character - 'a' + 10U;
            else if (character >= 'A' && character <= 'F') value |= character - 'A' + 10U;
            else return false;
        }
        return true;
    }

    bool finish(std::string& error) {
        skip_whitespace();
        return position_ == input_.size() || fail(error, "unexpected text after object");
    }

    bool consume(char expected) {
        if (position_ >= input_.size() || input_[position_] != expected) return false;
        ++position_;
        return true;
    }

    void skip_whitespace() {
        while (position_ < input_.size() &&
               std::isspace(static_cast<unsigned char>(input_[position_])) != 0) {
            ++position_;
        }
    }

    bool fail(std::string& error, const char* message) {
        error = std::string(message) + " at byte " + std::to_string(position_);
        return false;
    }

    std::string_view input_;
    std::size_t position_ = 0;
};

}  // namespace

std::optional<JsonFields> parse_flat_json(std::string_view line, std::string& error) {
    error.clear();
    JsonFields fields;
    JsonCursor cursor(line);
    if (!cursor.parse(fields, error)) return std::nullopt;
    return fields;
}

std::string escape_json_string(std::string_view value) {
    std::string output;
    output.reserve(value.size());
    for (const unsigned char character : value) {
        switch (character) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (character < 0x20U) {
                char escaped[8];
                std::snprintf(escaped, sizeof(escaped), "\\u%04x", static_cast<unsigned>(character));
                output += escaped;
            } else {
                output.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    return output;
}

std::string serialize_flat_json(const std::vector<std::pair<std::string, std::string>>& fields) {
    std::string output = "{";
    for (const auto& [name, value] : fields) {
        if (output.size() > 1U) output.push_back(',');
        output += "\"" + escape_json_string(name) + "\":\"" + escape_json_string(value) + "\"";
    }
    output.push_back('}');
    return output;
}

std::optional<NotificationMessage> parse_notification_json(std::string_view line,
                                                           std::string& error) {
    auto parsed = parse_flat_json(line, error);
    if (!parsed) return std::nullopt;
    const JsonFields& fields = *parsed;

    const auto type = fields.find("type");
    if (type == fields.end() || type->second != "notification") {
        error = "field 'type' must be 'notification'";
        return std::nullopt;
    }

    const auto required = [&fields, &error](const char* name) -> std::optional<std::string> {
        const auto found = fields.find(name);
        if (found == fields.end() || found->second.empty()) {
            error = std::string("missing non-empty field '") + name + "'";
            return std::nullopt;
        }
        return found->second;
    };

    NotificationMessage notification;
    auto id = required("id");
    auto app = required("app");
    auto title = required("title");
    auto message = required("message");
    auto timestamp = required("timestamp");
    if (!id || !app || !title || !message || !timestamp) return std::nullopt;
    notification.id = std::move(*id);
    notification.app = std::move(*app);
    notification.title = std::move(*title);
    notification.message = std::move(*message);
    notification.timestamp = std::move(*timestamp);
    if (const auto app_id = fields.find("app_id"); app_id != fields.end()) {
        notification.app_id = app_id->second;
    }
    const auto received_at = fields.find("received_at");
    if (received_at != fields.end()) {
        try {
            notification.received_at = std::stoll(received_at->second);
        } catch (const std::exception&) {
            error = "field 'received_at' must contain an integer";
            return std::nullopt;
        }
    } else {
        notification.received_at = std::chrono::duration_cast<std::chrono::seconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
    }
    const auto read = fields.find("read");
    notification.read = read != fields.end() && (read->second == "1" || read->second == "true");
    const auto dismissed = fields.find("dismissed");
    notification.dismissed =
        dismissed != fields.end() && (dismissed->second == "1" || dismissed->second == "true");
    const auto toast = fields.find("toast");
    notification.toast = toast == fields.end() ||
                         (toast->second != "0" && toast->second != "false");
    return notification;
}

}  // namespace frame_notify::ipc
