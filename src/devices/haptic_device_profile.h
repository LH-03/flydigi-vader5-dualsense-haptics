#pragma once

#include <string>

namespace vaderhaptics::devices {

enum class MotorCapability {
    Unsupported,
    Unknown,
    Supported,
};

struct HapticDeviceCapabilities {
    std::string deviceProfileId;
    std::string displayName;
    MotorCapability leftGrip = MotorCapability::Unknown;
    MotorCapability rightGrip = MotorCapability::Unknown;
    MotorCapability leftTriggerVibration = MotorCapability::Unknown;
    MotorCapability rightTriggerVibration = MotorCapability::Unknown;
    bool adaptiveTriggerResistance = false;
};

// Accepted firmware/owner evidence for the current physical target. This is a
// capability declaration, not renderer tuning or a transport implementation.
HapticDeviceCapabilities Vader5ProCapabilities();
bool HasCompleteFourLaneVibration(
    const HapticDeviceCapabilities& capabilities);

} // namespace vaderhaptics::devices
