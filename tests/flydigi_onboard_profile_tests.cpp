#include "input/flydigi_onboard_profile.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{

void Varint(std::vector<uint8_t>& out, uint64_t value)
{
    do {
        uint8_t byte = static_cast<uint8_t>(value & 0x7F);
        value >>= 7;
        if (value != 0) byte |= 0x80;
        out.push_back(byte);
    } while (value != 0);
}

void VarintField(std::vector<uint8_t>& out, uint32_t field, uint64_t value)
{
    Varint(out, static_cast<uint64_t>(field) << 3);
    Varint(out, value);
}

void BytesField(std::vector<uint8_t>& out, uint32_t field,
    const std::vector<uint8_t>& value)
{
    Varint(out, (static_cast<uint64_t>(field) << 3) | 2);
    Varint(out, value.size());
    out.insert(out.end(), value.begin(), value.end());
}

std::vector<uint8_t> ControllerTarget(uint64_t key)
{
    std::vector<uint8_t> target;
    VarintField(target, 1, key);
    return target;
}

std::vector<uint8_t> KeyboardTarget(uint64_t key)
{
    std::vector<uint8_t> target;
    VarintField(target, 2, key);
    return target;
}

std::vector<uint8_t> Entry(uint64_t source, uint64_t mapType,
    const std::vector<uint8_t>& target)
{
    std::vector<uint8_t> entry;
    VarintField(entry, 1, source);
    if (mapType != 0) VarintField(entry, 2, mapType);
    BytesField(entry, 3, target);
    return entry;
}

std::vector<uint8_t> Fixture()
{
    std::vector<uint8_t> mappings;
    BytesField(mappings, 1, Entry(16, 0, ControllerTarget(0)));
    BytesField(mappings, 1, Entry(18, 0, ControllerTarget(4)));
    BytesField(mappings, 1, Entry(19, 2, {}));
    BytesField(mappings, 1, Entry(20, 0, KeyboardTarget(90)));
    BytesField(mappings, 1, Entry(21, 0, ControllerTarget(26)));

    std::vector<uint8_t> profile;
    VarintField(profile, 1, 3);
    const std::string name = "slot3";
    BytesField(profile, 5,
        std::vector<uint8_t>(name.begin(), name.end()));
    BytesField(profile, 7, mappings);

    std::vector<uint8_t> top;
    BytesField(top, 1, profile);
    return top;
}

} // namespace

int main(int argc, char** argv)
{
    if (argc == 3) {
        std::ifstream file(argv[1], std::ios::binary);
        const std::vector<uint8_t> bytes{
            std::istreambuf_iterator<char>(file),
            std::istreambuf_iterator<char>()};
        vaderhaptics::input::FlydigiOnboardProfile inspected;
        std::string inspectError;
        const int slot = std::atoi(argv[2]);
        if (!file || slot < 0 || slot > 255 ||
            !vaderhaptics::input::ParseFlydigiOnboardProfile(
                bytes, static_cast<uint8_t>(slot), inspected, inspectError)) {
            std::printf("PROFILE_INSPECT FAIL %s\n", inspectError.c_str());
            return 1;
        }
        std::printf("PROFILE_INSPECT PASS slot=%u name=%s\n",
            inspected.slot, inspected.name.c_str());
        for (std::size_t index = 0;
             index < inspected.controllerTargets.size(); ++index) {
            std::printf("source=%zu target=%d\n", index + 16,
                inspected.controllerTargets[index]);
        }
        return 0;
    }

    int failures = 0;
    auto check = [&](bool condition, const char* name) {
        if (!condition) {
            std::printf("FAIL %s\n", name);
            ++failures;
        }
    };

    vaderhaptics::input::FlydigiOnboardProfile profile;
    std::string error;
    const auto fixture = Fixture();
    check(vaderhaptics::input::ParseFlydigiOnboardProfile(
              fixture, 3, profile, error),
        "parse selected profile");
    check(profile.slot == 3 && profile.name == "slot3", "identity");
    check(profile.controllerTargets[0] == 0, "C maps to dpad up");
    check(profile.controllerTargets[2] == 4, "M1 maps to A");
    check(profile.controllerTargets[3] == -1, "macro ignored");
    check(profile.controllerTargets[4] == -1, "keyboard ignored");
    check(profile.controllerTargets[5] == -1, "vendor target ignored");
    check(profile.ignoredNonControllerMappings == 2, "ignored type count");
    check(profile.ignoredUnsupportedTargets == 1,
        "unsupported target count");
    check(vaderhaptics::input::IsDs5RepresentableControllerKey(0) &&
          vaderhaptics::input::IsDs5RepresentableControllerKey(15) &&
          !vaderhaptics::input::IsDs5RepresentableControllerKey(16),
        "representable target boundary");

    check(!vaderhaptics::input::ParseFlydigiOnboardProfile(
              fixture, 2, profile, error),
        "missing active slot rejected");
    auto malformed = fixture;
    malformed.pop_back();
    check(!vaderhaptics::input::ParseFlydigiOnboardProfile(
              malformed, 3, profile, error),
        "truncated protobuf rejected");

    std::printf("FLYDIGI_ONBOARD_PROFILE_TESTS checks=11 failures=%d\n",
        failures);
    return failures == 0 ? 0 : 1;
}
