#include "runtime/session_state.h"

#include <charconv>
#include <fstream>
#include <string>
#include <string_view>

namespace vaderhaptics::runtime {
namespace {

bool ParseInt(std::string_view value, int& result)
{
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool ParseUint32(std::string_view value, uint32_t& result)
{
    const char* begin = value.data();
    const char* end = begin + value.size();
    const auto parsed = std::from_chars(begin, end, result);
    return parsed.ec == std::errc{} && parsed.ptr == end;
}

bool SafeStateString(const std::string& value)
{
    return value.find('\n') == std::string::npos &&
        value.find('\r') == std::string::npos && value.size() <= 4096;
}

} // namespace

bool WriteSessionState(const std::string& path, const SessionState& state,
    std::string& error)
{
    if (!SafeStateString(state.busId) || !SafeStateString(state.devId) ||
        !SafeStateString(state.physicalMi00ParentId) ||
        !SafeStateString(state.physicalGamePersonaId)) {
        error = "session state contains an invalid string";
        return false;
    }
    std::ofstream file(path, std::ios::trunc);
    if (!file) {
        error = "could not write session state: " + path;
        return false;
    }
    file << "busId=" << state.busId << "\n";
    file << "devId=" << state.devId << "\n";
    file << "usbipPort=" << state.usbipPort << "\n";
    file << "bridgePid=" << state.bridgePid << "\n";
    file << "serverPid=" << state.serverPid << "\n";
    file << "startedServer=" << (state.startedServer ? 1 : 0) << "\n";
    file << "physicalMi00ParentId=" << state.physicalMi00ParentId << "\n";
    file << "physicalGamePersonaId=" << state.physicalGamePersonaId << "\n";
    file << "physicalPersonaRestorePending="
         << (state.physicalPersonaRestorePending ? 1 : 0) << "\n";
    if (!file) {
        error = "session state write failed: " + path;
        return false;
    }
    error.clear();
    return true;
}

SessionStateReadResult ReadSessionState(const std::string& path,
    SessionState& state, std::string& error)
{
    std::ifstream file(path);
    if (!file) {
        error.clear();
        return SessionStateReadResult::NotFound;
    }

    SessionState parsed;
    bool sawBridgePid = false;
    std::string line;
    while (std::getline(file, line)) {
        const size_t eq = line.find('=');
        if (eq == std::string::npos) {
            error = "malformed session state line";
            return SessionStateReadResult::Invalid;
        }
        const std::string key = line.substr(0, eq);
        const std::string value = line.substr(eq + 1);
        if (key == "busId") parsed.busId = value;
        else if (key == "devId") parsed.devId = value;
        else if (key == "usbipPort") {
            if (!ParseInt(value, parsed.usbipPort)) {
                error = "invalid usbipPort in session state";
                return SessionStateReadResult::Invalid;
            }
        }
        else if (key == "bridgePid") {
            if (!ParseUint32(value, parsed.bridgePid)) {
                error = "invalid bridgePid in session state";
                return SessionStateReadResult::Invalid;
            }
            sawBridgePid = true;
        }
        else if (key == "serverPid") {
            if (!ParseUint32(value, parsed.serverPid)) {
                error = "invalid serverPid in session state";
                return SessionStateReadResult::Invalid;
            }
        }
        else if (key == "startedServer") {
            if (value != "0" && value != "1") {
                error = "invalid startedServer in session state";
                return SessionStateReadResult::Invalid;
            }
            parsed.startedServer = value == "1";
        }
        else if (key == "physicalMi00ParentId")
            parsed.physicalMi00ParentId = value;
        else if (key == "physicalGamePersonaId")
            parsed.physicalGamePersonaId = value;
        else if (key == "physicalPersonaRestorePending") {
            if (value != "0" && value != "1") {
                error = "invalid restore flag in session state";
                return SessionStateReadResult::Invalid;
            }
            parsed.physicalPersonaRestorePending = value == "1";
        }
    }

    if (!file.eof() || !sawBridgePid) {
        error = "incomplete session state";
        return SessionStateReadResult::Invalid;
    }
    state = std::move(parsed);
    error.clear();
    return SessionStateReadResult::Loaded;
}

} // namespace vaderhaptics::runtime
