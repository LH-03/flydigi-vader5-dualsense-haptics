#include "devices/haptic_device_profile.h"

namespace vaderhaptics::devices {

HapticDeviceCapabilities Vader5ProCapabilities()
{
    HapticDeviceCapabilities capabilities;
    capabilities.deviceProfileId = "flydigi-vader-5-pro-firmware-7.1.5.4";
    capabilities.displayName = "Flydigi Vader 5 Pro";
    capabilities.leftGrip = MotorCapability::Supported;
    capabilities.rightGrip = MotorCapability::Supported;
    capabilities.leftTriggerVibration = MotorCapability::Supported;
    capabilities.rightTriggerVibration = MotorCapability::Supported;
    capabilities.adaptiveTriggerResistance = false;
    return capabilities;
}

bool HasCompleteFourLaneVibration(
    const HapticDeviceCapabilities& capabilities)
{
    return capabilities.leftGrip == MotorCapability::Supported &&
        capabilities.rightGrip == MotorCapability::Supported &&
        capabilities.leftTriggerVibration == MotorCapability::Supported &&
        capabilities.rightTriggerVibration == MotorCapability::Supported;
}

} // namespace vaderhaptics::devices
