#include "runtime/mode_contract.h"

#include <algorithm>
#include <cctype>
#include <string>

namespace vaderhaptics::runtime {
namespace {

std::string Lower(std::string_view text)
{
    std::string value(text);
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

} // namespace

const char* RuntimeModeId(RuntimeMode mode)
{
    switch (mode) {
    case RuntimeMode::Native:
        return "native";
    case RuntimeMode::DualSense:
        return "ps5";
    case RuntimeMode::XboxEnhanced:
        return "xbox-enhanced";
    case RuntimeMode::Auto:
        return "auto";
    }
    return "native";
}

const char* RuntimeModeDisplayName(RuntimeMode mode)
{
    switch (mode) {
    case RuntimeMode::Native:
        return "Native / direct physical controller";
    case RuntimeMode::DualSense:
        return "PS5 / virtual DualSense";
    case RuntimeMode::XboxEnhanced:
        return "Xbox Enhanced / reserved";
    case RuntimeMode::Auto:
        return "Auto policy";
    }
    return "Native / direct physical controller";
}

bool ParseRuntimeMode(std::string_view text, RuntimeMode& mode)
{
    const std::string value = Lower(text);
    if (value == "native" || value == "direct" || value == "off") {
        mode = RuntimeMode::Native;
        return true;
    }
    if (value == "ps5" || value == "dualsense" || value == "ds5") {
        mode = RuntimeMode::DualSense;
        return true;
    }
    if (value == "xbox" || value == "xbox-enhanced" ||
        value == "xboxenhanced") {
        mode = RuntimeMode::XboxEnhanced;
        return true;
    }
    if (value == "auto") {
        mode = RuntimeMode::Auto;
        return true;
    }
    return false;
}

bool RuntimeModeOwnsHaptics(RuntimeMode mode)
{
    return mode == RuntimeMode::DualSense ||
        mode == RuntimeMode::XboxEnhanced;
}

ModeDecision ResolveRuntimeMode(RuntimeMode requested,
    const ModeAvailability& availability)
{
    ModeDecision decision;
    decision.requested = requested;

    switch (requested) {
    case RuntimeMode::Native:
        decision.effective = RuntimeMode::Native;
        decision.reason = "manual Native selection releases all haptic ownership";
        return decision;

    case RuntimeMode::DualSense:
        if (availability.dualSenseAvailable) {
            decision.effective = RuntimeMode::DualSense;
            decision.reason = "manual PS5 selection uses the DualSense runtime";
        }
        else {
            decision.effective = RuntimeMode::Native;
            decision.requestAvailable = false;
            decision.reason = "DualSense runtime is unavailable; fail closed to Native";
        }
        return decision;

    case RuntimeMode::XboxEnhanced:
        if (availability.xboxEnhancedAvailable) {
            decision.effective = RuntimeMode::XboxEnhanced;
            decision.reason = "manual Xbox Enhanced selection is available";
        }
        else {
            decision.effective = RuntimeMode::Native;
            decision.requestAvailable = false;
            decision.reason = "Xbox Enhanced is reserved; fail closed to Native";
        }
        return decision;

    case RuntimeMode::Auto:
        if (availability.gameKnownDualSense &&
            availability.dualSenseAvailable) {
            decision.effective = RuntimeMode::DualSense;
            decision.reason = "known-good DualSense game profile selected PS5 mode";
        }
        else if (availability.gameKnownXboxEnhanced &&
            availability.xboxEnhancedAvailable) {
            decision.effective = RuntimeMode::XboxEnhanced;
            decision.reason = "known-good Xbox Enhanced game profile selected Xbox mode";
        }
        else {
            decision.effective = RuntimeMode::Native;
            decision.reason = availability.gameKnownDualSense
                ? "DualSense profile matched but runtime is unavailable; use Native"
                : "unknown or unprofiled game defaults to Native";
        }
        return decision;
    }

    decision.effective = RuntimeMode::Native;
    decision.requestAvailable = false;
    decision.reason = "invalid mode fails closed to Native";
    return decision;
}

} // namespace vaderhaptics::runtime
