#pragma once

#include "runtime/mode_contract.h"

#include <string>
#include <string_view>

namespace vaderhaptics::profiles {

constexpr int kRuntimeProfileSchemaVersion = 1;

struct GameRuntimeProfile {
    int schemaVersion = kRuntimeProfileSchemaVersion;
    std::string executable;
    runtime::RuntimeMode requestedMode = runtime::RuntimeMode::Auto;
    bool dualSenseKnownGood = false;
    bool xboxEnhancedKnownGood = false;
    bool restartRequired = false;
    std::string tuningPreset = "default";
};

bool ParseGameRuntimeProfile(std::string_view json,
    GameRuntimeProfile& profile, std::string& error);
bool LoadGameRuntimeProfile(const std::string& path,
    GameRuntimeProfile& profile, std::string& error);
std::string SerializeGameRuntimeProfile(const GameRuntimeProfile& profile);

runtime::ModeAvailability AvailabilityForProfile(
    const GameRuntimeProfile& profile,
    bool dualSenseRuntimeAvailable = true,
    bool xboxEnhancedRuntimeAvailable = false);

} // namespace vaderhaptics::profiles
