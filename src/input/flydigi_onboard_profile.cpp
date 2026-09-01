#include "input/flydigi_onboard_profile.h"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <string_view>

namespace vaderhaptics::input
{
namespace
{

struct Field
{
    uint32_t number = 0;
    uint32_t wireType = 0;
    uint64_t value = 0;
    std::string_view bytes;
};

class MessageReader
{
public:
    explicit MessageReader(std::string_view bytes) : bytes_(bytes) {}

    bool Next(Field& field)
    {
        if (position_ == bytes_.size()) {
            return false;
        }
        uint64_t tag = 0;
        if (!ReadVarint(tag) || tag == 0) {
            failed_ = true;
            return false;
        }
        field = {};
        field.number = static_cast<uint32_t>(tag >> 3);
        field.wireType = static_cast<uint32_t>(tag & 7);
        switch (field.wireType) {
        case 0:
            if (!ReadVarint(field.value)) failed_ = true;
            break;
        case 1:
            if (!Skip(8)) failed_ = true;
            break;
        case 2: {
            uint64_t length = 0;
            if (!ReadVarint(length) ||
                length > static_cast<uint64_t>(bytes_.size() - position_)) {
                failed_ = true;
                break;
            }
            field.bytes = bytes_.substr(
                position_, static_cast<std::size_t>(length));
            position_ += static_cast<std::size_t>(length);
            break;
        }
        case 5:
            if (!Skip(4)) failed_ = true;
            break;
        default:
            failed_ = true;
            break;
        }
        return !failed_;
    }

    bool failed() const { return failed_; }

private:
    bool ReadVarint(uint64_t& value)
    {
        value = 0;
        for (uint32_t shift = 0; shift < 64 && position_ < bytes_.size();
             shift += 7) {
            const uint8_t byte =
                static_cast<uint8_t>(bytes_[position_++]);
            value |= static_cast<uint64_t>(byte & 0x7F) << shift;
            if ((byte & 0x80) == 0) return true;
        }
        return false;
    }

    bool Skip(std::size_t count)
    {
        if (count > bytes_.size() - position_) return false;
        position_ += count;
        return true;
    }

    std::string_view bytes_;
    std::size_t position_ = 0;
    bool failed_ = false;
};

bool ReadControllerTarget(std::string_view bytes, uint64_t& target)
{
    MessageReader reader(bytes);
    Field field;
    bool found = false;
    while (reader.Next(field)) {
        if (field.number == 1 && field.wireType == 0) {
            target = field.value;
            found = true;
        }
    }
    return !reader.failed() && found;
}

bool ReadMappings(std::string_view bytes, FlydigiOnboardProfile& profile)
{
    MessageReader list(bytes);
    Field item;
    while (list.Next(item)) {
        if (item.number != 1 || item.wireType != 2) continue;

        MessageReader entry(item.bytes);
        Field field;
        uint64_t source = 0;
        uint64_t mapType = 0; // KeyMapType_Key is the protobuf default.
        std::string_view targetBytes;
        while (entry.Next(field)) {
            if (field.number == 1 && field.wireType == 0) source = field.value;
            else if (field.number == 2 && field.wireType == 0) {
                mapType = field.value;
            }
            else if (field.number == 3 && field.wireType == 2) {
                targetBytes = field.bytes;
            }
        }
        if (entry.failed()) return false;
        if (source < 16 || source >= 16 + kFlydigiExpansionKeyCount) {
            continue;
        }
        const std::size_t index = static_cast<std::size_t>(source - 16);
        if (mapType != 0) {
            ++profile.ignoredNonControllerMappings;
            continue;
        }
        uint64_t target = 0;
        if (!ReadControllerTarget(targetBytes, target)) {
            ++profile.ignoredNonControllerMappings;
            continue;
        }
        if (!IsDs5RepresentableControllerKey(target)) {
            ++profile.ignoredUnsupportedTargets;
            continue;
        }
        profile.controllerTargets[index] = static_cast<int16_t>(target);
    }
    return !list.failed();
}

bool ReadProfile(std::string_view bytes, uint8_t requestedSlot,
    FlydigiOnboardProfile& profile, bool& selected)
{
    MessageReader reader(bytes);
    Field field;
    uint64_t slot = 0;
    std::string_view name;
    std::string_view mappings;
    while (reader.Next(field)) {
        if (field.number == 1 && field.wireType == 0) slot = field.value;
        else if (field.number == 5 && field.wireType == 2) name = field.bytes;
        else if (field.number == 7 && field.wireType == 2) {
            mappings = field.bytes;
        }
    }
    if (reader.failed()) return false;
    if (slot != requestedSlot) return true;
    if (slot > std::numeric_limits<uint8_t>::max() || mappings.empty()) {
        return false;
    }

    profile = {};
    profile.controllerTargets.fill(-1);
    profile.slot = static_cast<uint8_t>(slot);
    profile.name.assign(name.data(), name.size());
    selected = ReadMappings(mappings, profile);
    return selected;
}

} // namespace

bool IsDs5RepresentableControllerKey(uint64_t controllerKey)
{
    // Flydigi ControllerKey 0..15 are d-pad, face, select/start,
    // shoulders, digital triggers, and stick clicks. These have direct DS5
    // equivalents. Vendor, macro, keyboard/mouse, and synthetic directions do
    // not.
    return controllerKey <= 15;
}

bool ParseFlydigiOnboardProfile(const std::vector<uint8_t>& bytes,
    uint8_t activeSlot, FlydigiOnboardProfile& profile, std::string& error)
{
    profile = {};
    profile.controllerTargets.fill(-1);
    error.clear();
    if (bytes.empty()) {
        error = "Flydigi onboard profile file is empty";
        return false;
    }

    const std::string_view view(
        reinterpret_cast<const char*>(bytes.data()), bytes.size());
    MessageReader top(view);
    Field field;
    bool found = false;
    while (top.Next(field)) {
        if (field.number != 1 || field.wireType != 2) continue;
        if (!ReadProfile(field.bytes, activeSlot, profile, found)) {
            error = "Flydigi onboard profile protobuf is malformed";
            return false;
        }
        if (found) return true;
    }
    if (top.failed()) {
        error = "Flydigi onboard profile container is malformed";
    }
    else {
        error = "active Flydigi onboard profile slot was not found";
    }
    return false;
}

} // namespace vaderhaptics::input
