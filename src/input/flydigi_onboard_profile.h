#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace vaderhaptics::input
{

constexpr std::size_t kFlydigiExpansionKeyCount = 9;

struct FlydigiOnboardProfile
{
    uint8_t slot = 0;
    std::string name;
    // Raw sources 16..24 are C, Z, M1..M4, LM, RM, and the extra circle key.
    // A negative value means there is no representable controller-key target.
    std::array<int16_t, kFlydigiExpansionKeyCount> controllerTargets{};
    uint32_t ignoredNonControllerMappings = 0;
    uint32_t ignoredUnsupportedTargets = 0;
};

bool IsDs5RepresentableControllerKey(uint64_t controllerKey);

bool ParseFlydigiOnboardProfile(const std::vector<uint8_t>& bytes,
    uint8_t activeSlot, FlydigiOnboardProfile& profile, std::string& error);

} // namespace vaderhaptics::input
