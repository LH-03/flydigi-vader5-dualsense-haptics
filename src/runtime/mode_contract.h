#pragma once

#include <string>
#include <string_view>

namespace vaderhaptics::runtime {

enum class RuntimeMode {
    Native,
    DualSense,
    XboxEnhanced,
    Auto,
};

struct ModeAvailability {
    bool dualSenseAvailable = true;
    bool xboxEnhancedAvailable = false;
    bool gameKnownDualSense = false;
    bool gameKnownXboxEnhanced = false;
};

struct ModeDecision {
    RuntimeMode requested = RuntimeMode::Native;
    RuntimeMode effective = RuntimeMode::Native;
    bool requestAvailable = true;
    std::string reason;
};

const char* RuntimeModeId(RuntimeMode mode);
const char* RuntimeModeDisplayName(RuntimeMode mode);
bool ParseRuntimeMode(std::string_view text, RuntimeMode& mode);
bool RuntimeModeOwnsHaptics(RuntimeMode mode);
ModeDecision ResolveRuntimeMode(RuntimeMode requested,
    const ModeAvailability& availability);

} // namespace vaderhaptics::runtime
