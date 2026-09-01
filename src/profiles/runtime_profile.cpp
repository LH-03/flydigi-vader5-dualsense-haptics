#include "profiles/runtime_profile.h"

#include <cctype>
#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>

namespace vaderhaptics::profiles {
namespace {

struct JsonValue {
    enum class Type {
        String,
        Boolean,
        Integer,
    };

    Type type = Type::String;
    std::string stringValue;
    bool boolValue = false;
    int intValue = 0;
};

using JsonObject = std::unordered_map<std::string, JsonValue>;

void SkipSpace(std::string_view text, size_t& at)
{
    while (at < text.size() &&
        std::isspace(static_cast<unsigned char>(text[at]))) {
        ++at;
    }
}

bool ParseJsonString(std::string_view text, size_t& at, std::string& value,
    std::string& error)
{
    if (at >= text.size() || text[at] != '"') {
        error = "expected JSON string";
        return false;
    }
    ++at;
    std::string parsed;
    while (at < text.size()) {
        const unsigned char ch = static_cast<unsigned char>(text[at++]);
        if (ch == '"') {
            value = std::move(parsed);
            return true;
        }
        if (ch < 0x20) {
            error = "unescaped control character in JSON string";
            return false;
        }
        if (ch != '\\') {
            parsed.push_back(static_cast<char>(ch));
            continue;
        }
        if (at >= text.size()) {
            error = "unterminated JSON escape";
            return false;
        }
        const char escaped = text[at++];
        switch (escaped) {
        case '"': parsed.push_back('"'); break;
        case '\\': parsed.push_back('\\'); break;
        case '/': parsed.push_back('/'); break;
        case 'b': parsed.push_back('\b'); break;
        case 'f': parsed.push_back('\f'); break;
        case 'n': parsed.push_back('\n'); break;
        case 'r': parsed.push_back('\r'); break;
        case 't': parsed.push_back('\t'); break;
        default:
            error = "unsupported JSON string escape";
            return false;
        }
    }
    error = "unterminated JSON string";
    return false;
}

bool ParseJsonInteger(std::string_view text, size_t& at, int& value,
    std::string& error)
{
    bool negative = false;
    if (at < text.size() && text[at] == '-') {
        negative = true;
        ++at;
    }
    if (at >= text.size() ||
        !std::isdigit(static_cast<unsigned char>(text[at]))) {
        error = "expected JSON integer";
        return false;
    }
    int64_t parsed = 0;
    while (at < text.size() &&
        std::isdigit(static_cast<unsigned char>(text[at]))) {
        parsed = parsed * 10 + (text[at++] - '0');
        if (parsed > static_cast<int64_t>(std::numeric_limits<int>::max()) + 1) {
            error = "JSON integer is out of range";
            return false;
        }
    }
    if (at < text.size() &&
        (text[at] == '.' || text[at] == 'e' || text[at] == 'E')) {
        error = "runtime profile accepts integer fields only";
        return false;
    }
    const int64_t signedValue = negative ? -parsed : parsed;
    if (signedValue < std::numeric_limits<int>::min() ||
        signedValue > std::numeric_limits<int>::max()) {
        error = "JSON integer is out of range";
        return false;
    }
    value = static_cast<int>(signedValue);
    return true;
}

bool ParseFlatJsonObject(std::string_view text, JsonObject& object,
    std::string& error)
{
    size_t at = 0;
    SkipSpace(text, at);
    if (at >= text.size() || text[at++] != '{') {
        error = "runtime profile must be one JSON object";
        return false;
    }
    SkipSpace(text, at);
    if (at < text.size() && text[at] == '}') {
        ++at;
    }
    else {
        while (at < text.size()) {
            std::string key;
            if (!ParseJsonString(text, at, key, error)) {
                return false;
            }
            SkipSpace(text, at);
            if (at >= text.size() || text[at++] != ':') {
                error = "expected ':' after runtime profile field";
                return false;
            }
            SkipSpace(text, at);

            JsonValue value;
            if (at < text.size() && text[at] == '"') {
                value.type = JsonValue::Type::String;
                if (!ParseJsonString(text, at, value.stringValue, error)) {
                    return false;
                }
            }
            else if (text.substr(at, 4) == "true") {
                value.type = JsonValue::Type::Boolean;
                value.boolValue = true;
                at += 4;
            }
            else if (text.substr(at, 5) == "false") {
                value.type = JsonValue::Type::Boolean;
                value.boolValue = false;
                at += 5;
            }
            else {
                value.type = JsonValue::Type::Integer;
                if (!ParseJsonInteger(text, at, value.intValue, error)) {
                    return false;
                }
            }
            if (!object.emplace(key, std::move(value)).second) {
                error = "duplicate runtime profile field: " + key;
                return false;
            }

            SkipSpace(text, at);
            if (at >= text.size()) {
                error = "unterminated runtime profile object";
                return false;
            }
            if (text[at] == '}') {
                ++at;
                break;
            }
            if (text[at++] != ',') {
                error = "expected ',' between runtime profile fields";
                return false;
            }
            SkipSpace(text, at);
        }
    }
    SkipSpace(text, at);
    if (at != text.size()) {
        error = "unexpected data after runtime profile object";
        return false;
    }
    return true;
}

const JsonValue* Find(const JsonObject& object, std::string_view key)
{
    const auto found = object.find(std::string(key));
    return found == object.end() ? nullptr : &found->second;
}

bool ReadString(const JsonObject& object, std::string_view key,
    std::string& value, bool required, std::string& error)
{
    const JsonValue* field = Find(object, key);
    if (!field) {
        if (required) {
            error = "missing string field: " + std::string(key);
            return false;
        }
        return true;
    }
    if (field->type != JsonValue::Type::String) {
        error = "field is not a string: " + std::string(key);
        return false;
    }
    value = field->stringValue;
    return true;
}

bool ReadBool(const JsonObject& object, std::string_view key,
    bool& value, bool required, std::string& error)
{
    const JsonValue* field = Find(object, key);
    if (!field) {
        if (required) {
            error = "missing boolean field: " + std::string(key);
            return false;
        }
        return true;
    }
    if (field->type != JsonValue::Type::Boolean) {
        error = "field is not a boolean: " + std::string(key);
        return false;
    }
    value = field->boolValue;
    return true;
}

bool ReadInt(const JsonObject& object, std::string_view key,
    int& value, bool required, std::string& error)
{
    const JsonValue* field = Find(object, key);
    if (!field) {
        if (required) {
            error = "missing integer field: " + std::string(key);
            return false;
        }
        return true;
    }
    if (field->type != JsonValue::Type::Integer) {
        error = "field is not an integer: " + std::string(key);
        return false;
    }
    value = field->intValue;
    return true;
}

std::string EscapeJson(std::string_view value)
{
    std::string escaped;
    for (const char ch : value) {
        switch (ch) {
        case '"': escaped += "\\\""; break;
        case '\\': escaped += "\\\\"; break;
        case '\n': escaped += "\\n"; break;
        case '\r': escaped += "\\r"; break;
        case '\t': escaped += "\\t"; break;
        default: escaped.push_back(ch); break;
        }
    }
    return escaped;
}

bool IsSafeIdentifier(std::string_view value)
{
    if (value.empty() || value.size() > 128) {
        return false;
    }
    for (const unsigned char ch : value) {
        if (!(std::isalnum(ch) || ch == '.' || ch == '_' || ch == '-')) {
            return false;
        }
    }
    return true;
}

} // namespace

bool ParseGameRuntimeProfile(std::string_view json,
    GameRuntimeProfile& profile, std::string& error)
{
    JsonObject object;
    if (!ParseFlatJsonObject(json, object, error)) {
        return false;
    }

    GameRuntimeProfile parsed;
    std::string mode;
    if (!ReadInt(object, "schema_version", parsed.schemaVersion, true, error) ||
        !ReadString(object, "executable", parsed.executable, true, error) ||
        !ReadString(object, "mode", mode, true, error) ||
        !ReadBool(object, "dualsense_known_good", parsed.dualSenseKnownGood,
            false, error) ||
        !ReadBool(object, "xbox_enhanced_known_good",
            parsed.xboxEnhancedKnownGood, false, error) ||
        !ReadBool(object, "restart_required", parsed.restartRequired,
            false, error) ||
        !ReadString(object, "tuning_preset", parsed.tuningPreset,
            false, error)) {
        return false;
    }
    if (parsed.schemaVersion != kRuntimeProfileSchemaVersion) {
        error = "unsupported runtime profile schema_version";
        return false;
    }
    if (!runtime::ParseRuntimeMode(mode, parsed.requestedMode)) {
        error = "unsupported runtime mode: " + mode;
        return false;
    }
    if (!IsSafeIdentifier(parsed.executable)) {
        error = "executable must be a basename-style identifier";
        return false;
    }
    if (!IsSafeIdentifier(parsed.tuningPreset)) {
        error = "tuning_preset must be a safe identifier";
        return false;
    }
    if (parsed.requestedMode == runtime::RuntimeMode::XboxEnhanced &&
        !parsed.xboxEnhancedKnownGood) {
        error = "Xbox Enhanced profile must explicitly declare known-good support";
        return false;
    }
    profile = std::move(parsed);
    error.clear();
    return true;
}

bool LoadGameRuntimeProfile(const std::string& path,
    GameRuntimeProfile& profile, std::string& error)
{
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        error = "could not open runtime profile: " + path;
        return false;
    }
    std::ostringstream contents;
    contents << file.rdbuf();
    return ParseGameRuntimeProfile(contents.str(), profile, error);
}

std::string SerializeGameRuntimeProfile(const GameRuntimeProfile& profile)
{
    std::ostringstream json;
    json << "{\n"
         << "  \"schema_version\": " << profile.schemaVersion << ",\n"
         << "  \"executable\": \"" << EscapeJson(profile.executable)
         << "\",\n"
         << "  \"mode\": \"" << runtime::RuntimeModeId(profile.requestedMode)
         << "\",\n"
         << "  \"dualsense_known_good\": "
         << (profile.dualSenseKnownGood ? "true" : "false") << ",\n"
         << "  \"xbox_enhanced_known_good\": "
         << (profile.xboxEnhancedKnownGood ? "true" : "false") << ",\n"
         << "  \"restart_required\": "
         << (profile.restartRequired ? "true" : "false") << ",\n"
         << "  \"tuning_preset\": \""
         << EscapeJson(profile.tuningPreset) << "\"\n"
         << "}\n";
    return json.str();
}

runtime::ModeAvailability AvailabilityForProfile(
    const GameRuntimeProfile& profile, bool dualSenseRuntimeAvailable,
    bool xboxEnhancedRuntimeAvailable)
{
    runtime::ModeAvailability availability;
    availability.dualSenseAvailable = dualSenseRuntimeAvailable;
    availability.xboxEnhancedAvailable = xboxEnhancedRuntimeAvailable;
    availability.gameKnownDualSense = profile.dualSenseKnownGood;
    availability.gameKnownXboxEnhanced = profile.xboxEnhancedKnownGood;
    return availability;
}

} // namespace vaderhaptics::profiles
