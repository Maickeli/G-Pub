#include "gpub/config_loader.h"
#include "gpub/text_util.h"

#include <charconv>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <system_error>
#include <unordered_map>
#include <utility>
#include <vector>

namespace gpub {

namespace {

enum class JsonType {
    Null,
    Boolean,
    Number,
    String,
    Object,
    Array
};

struct JsonValue {
    JsonType type{JsonType::Null};
    bool bool_value{false};
    std::string scalar_value;
    std::unordered_map<std::string, JsonValue> object_value;
    std::vector<JsonValue> array_value;
};

void appendUtf8(std::string* out, std::uint32_t codepoint) {
    if (codepoint <= 0x7F) {
        out->push_back(static_cast<char>(codepoint));
        return;
    }
    if (codepoint <= 0x7FF) {
        out->push_back(static_cast<char>(0xC0 | (codepoint >> 6)));
        out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        return;
    }
    if (codepoint <= 0xFFFF) {
        out->push_back(static_cast<char>(0xE0 | (codepoint >> 12)));
        out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
        out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
        return;
    }
    out->push_back(static_cast<char>(0xF0 | (codepoint >> 18)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F)));
    out->push_back(static_cast<char>(0x80 | (codepoint & 0x3F)));
}

class JsonParser {
public:
    explicit JsonParser(std::string input)
        : input_(std::move(input)) {}

    JsonValue parse() {
        skipWhitespace();
        JsonValue root = parseValue();
        skipWhitespace();
        if (position_ != input_.size()) {
            fail("unexpected trailing characters");
        }
        return root;
    }

private:
    void skipWhitespace() {
        while (position_ < input_.size()) {
            if (std::isspace(static_cast<unsigned char>(input_[position_])) == 0) {
                break;
            }
            ++position_;
        }
    }

    [[noreturn]] void fail(const std::string& message) const {
        std::size_t line = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < position_ && i < input_.size(); ++i) {
            if (input_[i] == '\n') {
                ++line;
                column = 1;
            } else {
                ++column;
            }
        }
        throw std::runtime_error(
            "JSON parse error at line " + std::to_string(line) +
            ", column " + std::to_string(column) + ": " + message);
    }

    char peek() const {
        if (position_ >= input_.size()) {
            return '\0';
        }
        return input_[position_];
    }

    char consume() {
        if (position_ >= input_.size()) {
            fail("unexpected end of input");
        }
        return input_[position_++];
    }

    void expect(char expected) {
        const char actual = consume();
        if (actual != expected) {
            fail(std::string("expected '") + expected + "', got '" + actual + "'");
        }
    }

    JsonValue parseValue() {
        skipWhitespace();
        const char c = peek();
        if (c == '{') {
            return parseObject();
        }
        if (c == '[') {
            return parseArray();
        }
        if (c == '"') {
            JsonValue value{};
            value.type = JsonType::String;
            value.scalar_value = parseString();
            return value;
        }
        if (c == 't') {
            parseLiteral("true");
            JsonValue value{};
            value.type = JsonType::Boolean;
            value.bool_value = true;
            return value;
        }
        if (c == 'f') {
            parseLiteral("false");
            JsonValue value{};
            value.type = JsonType::Boolean;
            value.bool_value = false;
            return value;
        }
        if (c == 'n') {
            parseLiteral("null");
            JsonValue value{};
            value.type = JsonType::Null;
            return value;
        }
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c)) != 0) {
            JsonValue value{};
            value.type = JsonType::Number;
            value.scalar_value = parseNumber();
            return value;
        }
        fail("unexpected token");
    }

    JsonValue parseObject() {
        JsonValue value{};
        value.type = JsonType::Object;
        expect('{');
        skipWhitespace();
        if (peek() == '}') {
            consume();
            return value;
        }

        while (true) {
            skipWhitespace();
            if (peek() != '"') {
                fail("expected object key");
            }
            const std::string key = parseString();
            skipWhitespace();
            expect(':');
            JsonValue item = parseValue();
            value.object_value[key] = std::move(item);
            skipWhitespace();
            const char c = consume();
            if (c == '}') {
                break;
            }
            if (c != ',') {
                fail("expected ',' or '}' in object");
            }
        }
        return value;
    }

    JsonValue parseArray() {
        JsonValue value{};
        value.type = JsonType::Array;
        expect('[');
        skipWhitespace();
        if (peek() == ']') {
            consume();
            return value;
        }
        while (true) {
            value.array_value.push_back(parseValue());
            skipWhitespace();
            const char c = consume();
            if (c == ']') {
                break;
            }
            if (c != ',') {
                fail("expected ',' or ']' in array");
            }
        }
        return value;
    }

    std::string parseString() {
        expect('"');
        std::string out;
        while (position_ < input_.size()) {
            const char c = consume();
            if (c == '"') {
                return out;
            }
            if (c != '\\') {
                out.push_back(c);
                continue;
            }
            const char esc = consume();
            switch (esc) {
            case '"':
            case '\\':
            case '/':
                out.push_back(esc);
                break;
            case 'b':
                out.push_back('\b');
                break;
            case 'f':
                out.push_back('\f');
                break;
            case 'n':
                out.push_back('\n');
                break;
            case 'r':
                out.push_back('\r');
                break;
            case 't':
                out.push_back('\t');
                break;
            case 'u': {
                std::uint32_t codepoint = 0;
                for (int i = 0; i < 4; ++i) {
                    const char hex = consume();
                    codepoint <<= 4;
                    if (hex >= '0' && hex <= '9') {
                        codepoint |= static_cast<std::uint32_t>(hex - '0');
                    } else if (hex >= 'a' && hex <= 'f') {
                        codepoint |= static_cast<std::uint32_t>(hex - 'a' + 10);
                    } else if (hex >= 'A' && hex <= 'F') {
                        codepoint |= static_cast<std::uint32_t>(hex - 'A' + 10);
                    } else {
                        fail("invalid unicode escape");
                    }
                }
                appendUtf8(&out, codepoint);
                break;
            }
            default:
                fail("invalid string escape");
            }
        }
        fail("unterminated string");
    }

    void parseLiteral(const char* literal) {
        for (const char* p = literal; *p != '\0'; ++p) {
            if (consume() != *p) {
                fail(std::string("expected literal ") + literal);
            }
        }
    }

    std::string parseNumber() {
        const std::size_t start = position_;
        if (peek() == '-') {
            consume();
        }

        if (peek() == '0') {
            consume();
        } else {
            if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
                fail("invalid number");
            }
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                consume();
            }
        }

        if (peek() == '.') {
            consume();
            if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
                fail("invalid fraction in number");
            }
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                consume();
            }
        }

        if (peek() == 'e' || peek() == 'E') {
            consume();
            if (peek() == '+' || peek() == '-') {
                consume();
            }
            if (std::isdigit(static_cast<unsigned char>(peek())) == 0) {
                fail("invalid exponent in number");
            }
            while (std::isdigit(static_cast<unsigned char>(peek())) != 0) {
                consume();
            }
        }

        return input_.substr(start, position_ - start);
    }

    std::string input_;
    std::size_t position_{0};
};

const JsonValue* getObjectMember(const JsonValue& value, const std::string& key) {
    if (value.type != JsonType::Object) {
        return nullptr;
    }
    const auto it = value.object_value.find(key);
    if (it == value.object_value.end()) {
        return nullptr;
    }
    return &it->second;
}

bool tryReadString(const JsonValue& value, std::string* out) {
    if (value.type == JsonType::String || value.type == JsonType::Number) {
        *out = value.scalar_value;
        return true;
    }
    if (value.type == JsonType::Boolean) {
        *out = value.bool_value ? "true" : "false";
        return true;
    }
    return false;
}

bool tryReadInt(const JsonValue& value, std::int64_t* out) {
    if (value.type != JsonType::Number && value.type != JsonType::String) {
        return false;
    }
    const std::string& raw = value.scalar_value;
    std::int64_t parsed = 0;
    const auto [ptr, ec] = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
    if (ec != std::errc{} || ptr != raw.data() + raw.size()) {
        return false;
    }
    *out = parsed;
    return true;
}

std::string requireString(const JsonValue& value, const std::string& field_name) {
    std::string out;
    if (!tryReadString(value, &out)) {
        throw std::runtime_error("Expected string for field: " + field_name);
    }
    return out;
}

std::int64_t requireInt(const JsonValue& value, const std::string& field_name) {
    std::int64_t out = 0;
    if (!tryReadInt(value, &out)) {
        throw std::runtime_error("Expected integer for field: " + field_name);
    }
    return out;
}

void applyGlobalConfig(AppConfig& config, const JsonValue& global) {
    const JsonValue* debounce = getObjectMember(global, "debounce_ms");
    if (debounce != nullptr) {
        config.provider_debounce = std::chrono::milliseconds(requireInt(*debounce, "global.debounce_ms"));
    }

    const JsonValue* device_rate = getObjectMember(global, "device_rate_limit_ms");
    if (device_rate != nullptr) {
        config.device_rate_limit = std::chrono::milliseconds(requireInt(*device_rate, "global.device_rate_limit_ms"));
    }

    const JsonValue* fallback_poll = getObjectMember(global, "fallback_poll_interval_ms");
    if (fallback_poll != nullptr) {
        config.fallback_poll_interval = std::chrono::milliseconds(requireInt(*fallback_poll, "global.fallback_poll_interval_ms"));
    }

    const JsonValue* default_profile = getObjectMember(global, "default_profile");
    if (default_profile != nullptr) {
        config.default_profile = requireString(*default_profile, "global.default_profile");
    }

    const JsonValue* log_level = getObjectMember(global, "log_level");
    if (log_level != nullptr) {
        config.log_level = requireString(*log_level, "global.log_level");
    }
}

void loadProfiles(AppConfig& config, const JsonValue& profiles) {
    if (profiles.type != JsonType::Object) {
        throw std::runtime_error("Expected object for field: profiles");
    }

    for (const auto& profile_entry : profiles.object_value) {
        const std::string& profile_name = profile_entry.first;
        const JsonValue& profile_value = profile_entry.second;

        if (profile_value.type != JsonType::Object) {
            throw std::runtime_error("Expected object for profile: " + profile_name);
        }

        Profile profile{};
        profile.name = profile_name;

        for (const auto& profile_field : profile_value.object_value) {
            if (profile_field.first == "payloads") {
                continue;
            }
            std::string meta_value;
            if (tryReadString(profile_field.second, &meta_value)) {
                profile.payload_by_backend["meta"][profile_field.first] = meta_value;
            }
        }

        const JsonValue* payloads = getObjectMember(profile_value, "payloads");
        if (payloads != nullptr) {
            if (payloads->type != JsonType::Object) {
                throw std::runtime_error("Expected object for profile payloads: " + profile_name);
            }
            for (const auto& backend_entry : payloads->object_value) {
                const std::string& backend_id = backend_entry.first;
                const JsonValue& backend_payload = backend_entry.second;
                if (backend_payload.type != JsonType::Object) {
                    throw std::runtime_error("Expected payload object for profile: " + profile_name + ", backend: " + backend_id);
                }
                for (const auto& payload_field : backend_payload.object_value) {
                    std::string payload_value;
                    if (!tryReadString(payload_field.second, &payload_value)) {
                        throw std::runtime_error(
                            "Expected scalar payload value for profile: " + profile_name +
                            ", backend: " + backend_id + ", key: " + payload_field.first);
                    }
                    profile.payload_by_backend[backend_id][payload_field.first] = payload_value;
                }
            }
        }

        config.profiles[profile_name] = std::move(profile);
    }
}

void loadRules(AppConfig& config, const JsonValue& rules) {
    if (rules.type != JsonType::Array) {
        throw std::runtime_error("Expected array for field: rules");
    }

    config.rules.clear();
    config.rules.reserve(rules.array_value.size());

    for (std::size_t i = 0; i < rules.array_value.size(); ++i) {
        const JsonValue& raw_rule = rules.array_value[i];
        if (raw_rule.type != JsonType::Object) {
            throw std::runtime_error("Expected object in rules array at index " + std::to_string(i));
        }

        Rule rule{};
        rule.order = i;

        const JsonValue* profile_name = getObjectMember(raw_rule, "profile");
        if (profile_name == nullptr) {
            throw std::runtime_error("Missing required field rules[" + std::to_string(i) + "].profile");
        }
        rule.profile_name = requireString(*profile_name, "rules[" + std::to_string(i) + "].profile");

        const JsonValue* priority = getObjectMember(raw_rule, "priority");
        if (priority != nullptr) {
            rule.priority = static_cast<int>(requireInt(*priority, "rules[" + std::to_string(i) + "].priority"));
        }

        const JsonValue* executable_path = getObjectMember(raw_rule, "executable_path");
        if (executable_path != nullptr) {
            rule.executable_path_equals = requireString(*executable_path, "rules[" + std::to_string(i) + "].executable_path");
        }

        const JsonValue* process_name = getObjectMember(raw_rule, "process_name");
        if (process_name != nullptr) {
            rule.process_name_equals = requireString(*process_name, "rules[" + std::to_string(i) + "].process_name");
        }

        const JsonValue* title_regex = getObjectMember(raw_rule, "window_title_regex");
        if (title_regex != nullptr) {
            rule.window_title_regex = requireString(*title_regex, "rules[" + std::to_string(i) + "].window_title_regex");
        }

        config.rules.push_back(std::move(rule));
    }
}

} // namespace

AppConfig ConfigLoader::loadFromFile(const std::string& path) const {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open()) {
        throw std::runtime_error("Failed to open config: " + path);
    }

    std::ostringstream buffer;
    buffer << input.rdbuf();
    std::string raw = buffer.str();
    if (raw.empty()) {
        throw std::runtime_error("Config is empty: " + path);
    }

    JsonParser parser(std::move(raw));
    const JsonValue root = parser.parse();
    if (root.type != JsonType::Object) {
        throw std::runtime_error("Top-level JSON value must be an object.");
    }

    AppConfig config{};

    const JsonValue* global = getObjectMember(root, "global");
    if (global != nullptr) {
        if (global->type != JsonType::Object) {
            throw std::runtime_error("Expected object for field: global");
        }
        applyGlobalConfig(config, *global);
    }

    const JsonValue* profiles = getObjectMember(root, "profiles");
    if (profiles != nullptr) {
        loadProfiles(config, *profiles);
    }

    const JsonValue* rules = getObjectMember(root, "rules");
    if (rules != nullptr) {
        loadRules(config, *rules);
    }

    return config;
}

} // namespace gpub
