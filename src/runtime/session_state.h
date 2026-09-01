#pragma once

#include <cstdint>
#include <string>

namespace vaderhaptics::runtime {

struct SessionState {
    std::string busId;
    std::string devId;
    int usbipPort = -1;
    uint32_t bridgePid = 0;
    uint32_t serverPid = 0;
    bool startedServer = false;
    std::string physicalMi00ParentId;
    std::string physicalGamePersonaId;
    bool physicalPersonaRestorePending = false;
};

enum class SessionStateReadResult {
    NotFound,
    Loaded,
    Invalid,
};

bool WriteSessionState(const std::string& path, const SessionState& state,
    std::string& error);
SessionStateReadResult ReadSessionState(const std::string& path,
    SessionState& state, std::string& error);

} // namespace vaderhaptics::runtime
