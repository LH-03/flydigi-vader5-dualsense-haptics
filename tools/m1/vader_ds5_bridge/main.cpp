#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <Xinput.h>
#include <cfgmgr32.h>
#include <setupapi.h>

#include "m0/vaderhid.h"
#include "m2/capture_analysis.h"
#include "m2/input_marker_detector.h"
#include "m2/vpcm_parser.h"
#include "m3/haptics.h"
#include "devices/haptic_device_profile.h"
#include "runtime/mode_contract.h"
#include "runtime/session_state.h"
#include "profiles/runtime_profile.h"
#include "input/flydigi_onboard_profile.h"
#include "dpad_map.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <cwctype>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "xinput9_1_0.lib")
#pragma comment(lib, "cfgmgr32.lib")
#pragma comment(lib, "setupapi.lib")

namespace
{
constexpr const char* kApiHost = "127.0.0.1";
// App-owned endpoints, independent of VIIPER's commonly reserved defaults.
constexpr int kApiPort = 43242;
constexpr int kUsbPort = 43241;
constexpr const char* kDeviceType = "dualsensecombinedaudioduplexv5";
constexpr const char* kBridgeStopEventName =
    "Local\\VaderHapticsBridgeGracefulStop";
constexpr int64_t kInputNeutralizeLimitUs = 120000;
constexpr int64_t kInputDisconnectLimitUs = 2000000;

constexpr bool InputStreamNeedsNeutral(int64_t ageUs)
{
    return ageUs > kInputNeutralizeLimitUs;
}

constexpr bool InputStreamMustStop(int64_t ageUs)
{
    return ageUs > kInputDisconnectLimitUs;
}

std::string g_viiperExe;
bool g_nodelay = false;
bool g_inputDiagnostics = false;
HANDLE g_bridgeStopEvent = nullptr;

// DS5 payload constants (VIIPER device/dualsense/state.go + const.go).
constexpr uint8_t kTouchInactive = 0x80;
constexpr uint32_t kBtnSquare = 0x0010;
constexpr uint32_t kBtnCross = 0x0020;
constexpr uint32_t kBtnCircle = 0x0040;
constexpr uint32_t kBtnTriangle = 0x0080;
constexpr uint32_t kBtnL1 = 0x0100;
constexpr uint32_t kBtnR1 = 0x0200;
constexpr uint32_t kBtnL2 = 0x0400;
constexpr uint32_t kBtnR2 = 0x0800;
constexpr uint32_t kBtnCreate = 0x1000;
constexpr uint32_t kBtnOptions = 0x2000;
constexpr uint32_t kBtnL3 = 0x4000;
constexpr uint32_t kBtnR3 = 0x8000;
constexpr uint32_t kBtnTouchpad = 0x00020000;

volatile bool g_interrupted = false;

BOOL WINAPI ConsoleHandler(DWORD event)
{
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT) {
        g_interrupted = true;
        return TRUE;
    }
    return FALSE;
}

std::wstring Uppercase(std::wstring value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t ch) { return static_cast<wchar_t>(std::towupper(ch)); });
    return value;
}

std::string NarrowAscii(const std::wstring& value)
{
    std::string result;
    result.reserve(value.size());
    for (const wchar_t ch : value) {
        result.push_back(static_cast<char>(ch & 0x7F));
    }
    return result;
}

std::wstring WidenAscii(const std::string& value)
{
    return std::wstring(value.begin(), value.end());
}

std::vector<std::wstring> ResolveCurrentVaderMi00Instances(bool presentOnly)
{
    const DWORD flags = DIGCF_ALLCLASSES | (presentOnly ? DIGCF_PRESENT : 0);
    HDEVINFO set = SetupDiGetClassDevsW(nullptr, nullptr, nullptr, flags);
    if (set == INVALID_HANDLE_VALUE) {
        return {};
    }
    std::vector<std::wstring> matches;
    SP_DEVINFO_DATA data = {};
    data.cbSize = sizeof(data);
    for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &data); ++index) {
        wchar_t instance[MAX_DEVICE_ID_LEN] = {};
        if (!SetupDiGetDeviceInstanceIdW(
                set, &data, instance, MAX_DEVICE_ID_LEN, nullptr)) {
            continue;
        }
        const std::wstring upper = Uppercase(instance);
        if (upper.find(L"USB\\VID_37D7&PID_2401&MI_00\\") == 0) {
            matches.emplace_back(instance);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return matches;
}

std::vector<std::wstring> ResolveDirectGameChildren(
    const std::wstring& mi00InstanceId)
{
    DEVINST parent = 0;
    std::wstring mutableId = mi00InstanceId;
    if (CM_Locate_DevNodeW(
            &parent, mutableId.data(), CM_LOCATE_DEVNODE_NORMAL) !=
        CR_SUCCESS) {
        return {};
    }
    DEVINST child = 0;
    if (CM_Get_Child(&child, parent, 0) != CR_SUCCESS) {
        return {};
    }
    std::vector<std::wstring> matches;
    while (true) {
        wchar_t instance[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(child, instance, MAX_DEVICE_ID_LEN, 0) ==
            CR_SUCCESS) {
            const std::wstring upper = Uppercase(instance);
            if (upper.find(L"USB\\VID_37D7&PID_2401&IG_") == 0) {
                matches.emplace_back(instance);
            }
        }
        DEVINST sibling = 0;
        if (CM_Get_Sibling(&sibling, child, 0) != CR_SUCCESS) {
            break;
        }
        child = sibling;
    }
    return matches;
}

std::vector<std::wstring> ResolveDirectGameHidChildren(
    const std::wstring& gameChildInstanceId)
{
    DEVINST parent = 0;
    std::wstring mutableId = gameChildInstanceId;
    if (CM_Locate_DevNodeW(
            &parent, mutableId.data(), CM_LOCATE_DEVNODE_NORMAL) !=
        CR_SUCCESS) {
        return {};
    }
    DEVINST child = 0;
    if (CM_Get_Child(&child, parent, 0) != CR_SUCCESS) {
        return {};
    }
    std::vector<std::wstring> matches;
    while (true) {
        wchar_t instance[MAX_DEVICE_ID_LEN] = {};
        if (CM_Get_Device_IDW(child, instance, MAX_DEVICE_ID_LEN, 0) ==
            CR_SUCCESS) {
            const std::wstring upper = Uppercase(instance);
            if (upper.find(L"HID\\VID_37D7&PID_2401&IG_") == 0) {
                matches.emplace_back(instance);
            }
        }
        DEVINST sibling = 0;
        if (CM_Get_Sibling(&sibling, child, 0) != CR_SUCCESS) {
            break;
        }
        child = sibling;
    }
    return matches;
}

bool IsDeviceStarted(const std::wstring& instanceId, bool& started)
{
    DEVINST node = 0;
    std::wstring mutableId = instanceId;
    CONFIGRET code = CM_Locate_DevNodeW(
        &node, mutableId.data(), CM_LOCATE_DEVNODE_NORMAL);
    if (code != CR_SUCCESS) {
        return false;
    }
    ULONG status = 0;
    ULONG problem = 0;
    code = CM_Get_DevNode_Status(&status, &problem, node, 0);
    if (code != CR_SUCCESS) {
        return false;
    }
    started = (status & DN_STARTED) != 0;
    return true;
}

bool SetDeviceStarted(const std::wstring& instanceId, bool enable)
{
    DEVINST node = 0;
    std::wstring mutableId = instanceId;
    CONFIGRET code = CM_Locate_DevNodeW(
        &node, mutableId.data(), CM_LOCATE_DEVNODE_NORMAL);
    if (code != CR_SUCCESS) {
        std::printf("ERROR: CM_Locate_DevNode failed for %s (CR=%lu)\n",
            NarrowAscii(instanceId).c_str(), static_cast<unsigned long>(code));
        return false;
    }
    code = enable ? CM_Enable_DevNode(node, 0) :
        CM_Disable_DevNode(node, CM_DISABLE_POLITE);
    if (code != CR_SUCCESS) {
        std::printf("ERROR: CM_%s_DevNode failed for %s (CR=%lu).\n",
            enable ? "Enable" : "Disable", NarrowAscii(instanceId).c_str(),
            static_cast<unsigned long>(code));
        return false;
    }
    for (int attempt = 0; attempt < 50; ++attempt) {
        bool started = false;
        if (IsDeviceStarted(instanceId, started) && started == enable) {
            std::printf("PnP %s confirmed: %s\n",
                enable ? "enable" : "disable", NarrowAscii(instanceId).c_str());
            return true;
        }
        Sleep(100);
    }
    std::printf("ERROR: PnP %s did not reach requested state: %s\n",
        enable ? "enable" : "disable", NarrowAscii(instanceId).c_str());
    return false;
}

std::string ExpandEnv(const std::string& path)
{
    std::vector<char> buffer(4096);
    DWORD size = ExpandEnvironmentStringsA(
        path.c_str(), buffer.data(), static_cast<DWORD>(buffer.size()));
    if (size == 0 || size > buffer.size()) {
        return path;
    }
    return std::string(buffer.data(), size);
}

std::string GetStatePath()
{
    return ExpandEnv("%TEMP%\\vader_ds5_bridge_state.txt");
}

bool ReadAll(SOCKET sock, std::string& out, int timeoutMs)
{
    out.clear();
    char buffer[8192];
    DWORD timeout = static_cast<DWORD>(timeoutMs);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&timeout), sizeof(timeout));
    while (true) {
        int n = recv(sock, buffer, sizeof(buffer), 0);
        if (n > 0) {
            out.append(buffer, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            break; // server closed the response
        }
        int err = WSAGetLastError();
        if (err == WSAETIMEDOUT || err == WSAEWOULDBLOCK) {
            break;
        }
        return false;
    }
    return true;
}

bool ConnectWithTimeout(SOCKET sock, const sockaddr_in& address, int timeoutMs)
{
    u_long nonblocking = 1;
    if (ioctlsocket(sock, FIONBIO, &nonblocking) != 0) return false;
    bool connected = connect(sock, reinterpret_cast<const sockaddr*>(&address),
        sizeof(address)) == 0;
    if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
        fd_set writable, errors;
        FD_ZERO(&writable);
        FD_ZERO(&errors);
        FD_SET(sock, &writable);
        FD_SET(sock, &errors);
        timeval timeout = {timeoutMs / 1000, (timeoutMs % 1000) * 1000};
        if (select(0, nullptr, &writable, &errors, &timeout) > 0) {
            int error = 0;
            int size = sizeof(error);
            connected = getsockopt(sock, SOL_SOCKET, SO_ERROR,
                reinterpret_cast<char*>(&error), &size) == 0 && error == 0
                && FD_ISSET(sock, &writable);
        }
    }
    nonblocking = 0;
    return ioctlsocket(sock, FIONBIO, &nonblocking) == 0 && connected;
}

bool ApiRequest(const std::string& request, std::string& response,
    int responseTimeoutMs = 3000)
{
    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return false;
    }
    DWORD sendTimeout = 2000;
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kApiPort);
    inet_pton(AF_INET, kApiHost, &addr.sin_addr);

    bool ok = false;
    if (ConnectWithTimeout(sock, addr, 500)) {
        std::string wire = request;
        wire.push_back('\0');
        int sent = send(sock, wire.data(), static_cast<int>(wire.size()), 0);
        if (sent == static_cast<int>(wire.size())) {
            ok = ReadAll(sock, response, responseTimeoutMs);
        }
    }
    closesocket(sock);
    return ok;
}

std::string FindJsonValue(const std::string& json, const char* field)
{
    std::string key = std::string("\"") + field + "\"";
    size_t pos = json.find(key);
    if (pos == std::string::npos) {
        return {};
    }
    pos = json.find(':', pos + key.size());
    if (pos == std::string::npos) {
        return {};
    }
    ++pos;
    while (pos < json.size() &&
           (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' ||
               json[pos] == '\n')) {
        ++pos;
    }
    if (pos >= json.size()) {
        return {};
    }
    if (json[pos] == '"') {
        ++pos;
        std::string value;
        while (pos < json.size() && json[pos] != '"') {
            if (json[pos] == '\\' && pos + 1 < json.size()) {
                ++pos;
            }
            value.push_back(json[pos]);
            ++pos;
        }
        return value;
    }
    size_t end = pos;
    while (end < json.size() && json[end] != ',' && json[end] != '}' &&
           json[end] != '\r' && json[end] != '\n') {
        ++end;
    }
    return json.substr(pos, end - pos);
}

uint32_t Crc32(const uint8_t* data, size_t size)
{
    static uint32_t table[256] = {};
    static bool initialized = false;
    if (!initialized) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t crc = i;
            for (int j = 0; j < 8; ++j) {
                crc = (crc & 1) ? ((crc >> 1) ^ 0xEDB88320u) : (crc >> 1);
            }
            table[i] = crc;
        }
        initialized = true;
    }
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < size; ++i) {
        crc = (crc >> 8) ^ table[(crc ^ data[i]) & 0xFFu];
    }
    return crc ^ 0xFFFFFFFFu;
}

std::array<uint8_t, 49> BuildVpcmInputFrame(uint32_t sequence,
    const uint8_t payload[33])
{
    std::array<uint8_t, 49> frame{};
    frame[0] = 'V';
    frame[1] = 'P';
    frame[2] = 'C';
    frame[3] = 'M';
    frame[4] = 0x05; // version
    frame[5] = 0x01; // input state
    frame[6] = 33;
    frame[7] = 0;
    frame[8] = static_cast<uint8_t>(sequence & 0xFF);
    frame[9] = static_cast<uint8_t>((sequence >> 8) & 0xFF);
    frame[10] = static_cast<uint8_t>((sequence >> 16) & 0xFF);
    frame[11] = static_cast<uint8_t>((sequence >> 24) & 0xFF);
    std::memcpy(frame.data() + 16, payload, 33);
    std::vector<uint8_t> crcInput(8 + 33);
    std::memcpy(crcInput.data(), frame.data() + 4, 8);
    std::memcpy(crcInput.data() + 8, payload, 33);
    uint32_t crc = Crc32(crcInput.data(), crcInput.size());
    frame[12] = static_cast<uint8_t>(crc & 0xFF);
    frame[13] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    frame[14] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    frame[15] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    return frame;
}

int16_t Load16LE(const uint8_t* data)
{
    return static_cast<int16_t>(static_cast<uint16_t>(data[0]) |
                                (static_cast<uint16_t>(data[1]) << 8));
}

struct VaderState
{
    int16_t lx = 0;
    int16_t ly = 0;
    int16_t rx = 0;
    int16_t ry = 0;
    uint8_t hat = 0;
    uint16_t face = 0; // raw standard button bytes 11/12, guide excluded
    uint8_t vendorButtons = 0; // raw byte 13: C/Z, M1-M4, LM/RM
    uint8_t vendorExtraButtons = 0; // raw byte 14: extra circle key bit 0
    uint8_t l2 = 0;
    uint8_t r2 = 0;
};

// Replaceable ordinary-input boundary. Axis values retain the mature XInput
// range and convention (signed 16-bit; positive Y is up). No deadzone or
// response curve is applied here. DS5 Y inversion happens only at rendering.
struct NormalizedInputState
{
    int16_t lx = 0;
    int16_t ly = 0;
    int16_t rx = 0;
    int16_t ry = 0;
    uint8_t dpad = 0; // up=0x01, down=0x02, left=0x04, right=0x08
    uint8_t vendorButtons = 0; // diagnostics only; never mapped directly
    uint8_t vendorExtraButtons = 0; // diagnostics and source 24
    uint8_t l2 = 0;
    uint8_t r2 = 0;
    bool a = false;
    bool b = false;
    bool x = false;
    bool y = false;
    bool l1 = false;
    bool r1 = false;
    bool l3 = false;
    bool r3 = false;
    bool back = false;
    bool start = false;
};

struct InputCoverage
{
    uint64_t stateChanges = 0;
    uint32_t buttonsSeen = 0;
    uint8_t dpadSeen = 0;
    uint8_t vendorButtonsSeen = 0;
    int16_t lxMin = 32767;
    int16_t lxMax = -32768;
    int16_t lyMin = 32767;
    int16_t lyMax = -32768;
    int16_t rxMin = 32767;
    int16_t rxMax = -32768;
    int16_t ryMin = 32767;
    int16_t ryMax = -32768;
    uint8_t l2Max = 0;
    uint8_t r2Max = 0;
};

void UpdateInputCoverage(InputCoverage& coverage,
    const NormalizedInputState& input, uint32_t mappedButtons,
    bool stateChanged)
{
    if (stateChanged) {
        ++coverage.stateChanges;
    }
    coverage.buttonsSeen |= mappedButtons;
    coverage.dpadSeen |= input.dpad;
    coverage.vendorButtonsSeen |= input.vendorButtons;
    coverage.lxMin = std::min(coverage.lxMin, input.lx);
    coverage.lxMax = std::max(coverage.lxMax, input.lx);
    coverage.lyMin = std::min(coverage.lyMin, input.ly);
    coverage.lyMax = std::max(coverage.lyMax, input.ly);
    coverage.rxMin = std::min(coverage.rxMin, input.rx);
    coverage.rxMax = std::max(coverage.rxMax, input.rx);
    coverage.ryMin = std::min(coverage.ryMin, input.ry);
    coverage.ryMax = std::max(coverage.ryMax, input.ry);
    coverage.l2Max = std::max(coverage.l2Max, input.l2);
    coverage.r2Max = std::max(coverage.r2Max, input.r2);
}

bool DecodeVaderReport(const uint8_t* data, size_t size, VaderState& out)
{
    if (size > 0 && data[0] != 0x5A) {
        ++data;
        --size;
    }
    if (size < 31 || data[0] != 0x5A || data[1] != 0xA5 ||
        data[2] != 0xEF) {
        return false;
    }
    out.lx = Load16LE(data + 3);
    int16_t lyRaw = Load16LE(data + 5);
    out.ly = static_cast<int16_t>(-static_cast<int>(lyRaw));
    if (out.ly == -32768) {
        out.ly = 32767;
    }
    out.rx = Load16LE(data + 7);
    int16_t ryRaw = Load16LE(data + 9);
    out.ry = static_cast<int16_t>(-static_cast<int>(ryRaw));
    if (out.ry == -32768) {
        out.ry = 32767;
    }
    out.hat = data[11] & 0x0F;
    out.face = static_cast<uint16_t>(data[11] | (data[12] << 8));
    out.vendorButtons = data[13];
    out.vendorExtraButtons = data[14];
    out.l2 = data[15];
    out.r2 = data[16];
    return true;
}

uint8_t ToS8(int16_t value)
{
    return static_cast<uint8_t>(static_cast<int8_t>(value >> 8));
}

uint8_t DpadBits(uint8_t hat)
{
    return MapVaderHatToDs5Dpad(hat);
}

int16_t InvertAxis(int16_t value)
{
    return value == -32768
        ? 32767
        : static_cast<int16_t>(-static_cast<int>(value));
}

NormalizedInputState NormalizeXInputState(const XINPUT_GAMEPAD& gamepad);

NormalizedInputState NormalizeMi01State(const VaderState& v)
{
    NormalizedInputState out;
    out.lx = v.lx;
    // DecodeVaderReport historically converts Y to DS5 screen coordinates.
    // Convert it back at the source boundary so every backend exposes the
    // XInput convention and MapToDs5 owns the single output-side inversion.
    out.ly = InvertAxis(v.ly);
    out.rx = v.rx;
    out.ry = InvertAxis(v.ry);
    out.dpad = DpadBits(v.hat);
    out.vendorButtons = v.vendorButtons;
    out.vendorExtraButtons = v.vendorExtraButtons;
    out.l2 = v.l2;
    out.r2 = v.r2;
    out.a = (v.face & 0x0010) != 0;
    out.b = (v.face & 0x0020) != 0;
    out.x = (v.face & 0x0080) != 0;
    out.y = (v.face & 0x0100) != 0;
    out.l1 = (v.face & 0x0400) != 0;
    out.r1 = (v.face & 0x0800) != 0;
    out.l3 = (v.face & 0x4000) != 0;
    out.r3 = (v.face & 0x8000) != 0;
    out.back = (v.face & 0x0040) != 0;
    out.start = (v.face & 0x0200) != 0;
    return out;
}

const char* FlydigiControllerKeyName(int16_t key)
{
    static constexpr const char* kNames[] = {
        "Up", "Right", "Down", "Left", "A", "B", "Select", "X",
        "Y", "Start", "LB", "RB", "LT", "RT", "L3", "R3"};
    return key >= 0 && key < static_cast<int16_t>(std::size(kNames))
        ? kNames[key] : "unsupported";
}

void ApplyFlydigiExpansionKeys(
    const vaderhaptics::input::FlydigiOnboardProfile& profile,
    NormalizedInputState& state)
{
    for (std::size_t index = 0;
         index < vaderhaptics::input::kFlydigiExpansionKeyCount; ++index) {
        const bool pressed = index < 8
            ? (state.vendorButtons & (1u << index)) != 0
            : (state.vendorExtraButtons & 0x01) != 0;
        if (!pressed) continue;
        switch (profile.controllerTargets[index]) {
        case 0: state.dpad |= 0x01; break;
        case 1: state.dpad |= 0x08; break;
        case 2: state.dpad |= 0x02; break;
        case 3: state.dpad |= 0x04; break;
        case 4: state.a = true; break;
        case 5: state.b = true; break;
        case 6: state.back = true; break;
        case 7: state.x = true; break;
        case 8: state.y = true; break;
        case 9: state.start = true; break;
        case 10: state.l1 = true; break;
        case 11: state.r1 = true; break;
        case 12: state.l2 = 255; break;
        case 13: state.r2 = 255; break;
        case 14: state.l3 = true; break;
        case 15: state.r3 = true; break;
        default: break;
        }
    }
}

NormalizedInputState NormalizeXInputState(const XINPUT_GAMEPAD& gamepad)
{
    NormalizedInputState out;
    out.lx = gamepad.sThumbLX;
    out.ly = gamepad.sThumbLY;
    out.rx = gamepad.sThumbRX;
    out.ry = gamepad.sThumbRY;
    out.dpad = static_cast<uint8_t>(gamepad.wButtons &
        (XINPUT_GAMEPAD_DPAD_UP | XINPUT_GAMEPAD_DPAD_DOWN |
         XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT));
    out.l2 = gamepad.bLeftTrigger;
    out.r2 = gamepad.bRightTrigger;
    out.a = (gamepad.wButtons & XINPUT_GAMEPAD_A) != 0;
    out.b = (gamepad.wButtons & XINPUT_GAMEPAD_B) != 0;
    out.x = (gamepad.wButtons & XINPUT_GAMEPAD_X) != 0;
    out.y = (gamepad.wButtons & XINPUT_GAMEPAD_Y) != 0;
    out.l1 = (gamepad.wButtons & XINPUT_GAMEPAD_LEFT_SHOULDER) != 0;
    out.r1 = (gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_SHOULDER) != 0;
    out.l3 = (gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0;
    out.r3 = (gamepad.wButtons & XINPUT_GAMEPAD_RIGHT_THUMB) != 0;
    out.back = (gamepad.wButtons & XINPUT_GAMEPAD_BACK) != 0;
    out.start = (gamepad.wButtons & XINPUT_GAMEPAD_START) != 0;
    return out;
}

void MapToDs5(const NormalizedInputState& input, uint8_t payload[33])
{
    std::memset(payload, 0, 33);
    payload[0] = ToS8(input.lx);
    payload[1] = ToS8(InvertAxis(input.ly));
    payload[2] = ToS8(input.rx);
    payload[3] = ToS8(InvertAxis(input.ry));

    uint32_t buttons = 0;
    if (input.a) buttons |= kBtnCross;
    if (input.b) buttons |= kBtnCircle;
    if (input.x) buttons |= kBtnSquare;
    if (input.y) buttons |= kBtnTriangle;
    if (input.l1) buttons |= kBtnL1;
    if (input.r1) buttons |= kBtnR1;
    if (input.l3) buttons |= kBtnL3;
    if (input.r3) buttons |= kBtnR3;
    // Xbox View is the in-game secondary menu button. On the DS5 persona,
    // expose its basic touchpad click (no touch coordinates or gestures).
    if (input.back) buttons |= kBtnTouchpad;
    if (input.start) buttons |= kBtnOptions;
    if (input.l2 > 0) buttons |= kBtnL2;
    if (input.r2 > 0) buttons |= kBtnR2;

    payload[4] = static_cast<uint8_t>(buttons & 0xFF);
    payload[5] = static_cast<uint8_t>((buttons >> 8) & 0xFF);
    payload[6] = static_cast<uint8_t>((buttons >> 16) & 0xFF);
    payload[7] = static_cast<uint8_t>((buttons >> 24) & 0xFF);
    payload[8] = input.dpad;
    payload[9] = input.l2;
    payload[10] = input.r2;
    payload[15] = kTouchInactive;
    payload[20] = kTouchInactive;
    // Accel: neutral 1g down, matching VIIPER NewInputState().
    payload[29] = 0x00;
    payload[30] = 0x00;
    payload[31] = static_cast<uint8_t>(0xE000 & 0xFF);
    payload[32] = static_cast<uint8_t>((0xE000 >> 8) & 0xFF);
}

void MapToDs5(const VaderState& v, uint8_t payload[33])
{
    MapToDs5(NormalizeMi01State(v), payload);
}

int RunInputMappingSelfTest()
{
    XINPUT_GAMEPAD gamepad = {};
    gamepad.sThumbLX = -32768;
    gamepad.sThumbLY = 32767;
    gamepad.sThumbRX = 32767;
    gamepad.sThumbRY = -32768;
    gamepad.bLeftTrigger = 1;
    gamepad.bRightTrigger = 255;
    gamepad.wButtons = XINPUT_GAMEPAD_DPAD_UP |
        XINPUT_GAMEPAD_DPAD_RIGHT |
        XINPUT_GAMEPAD_START | XINPUT_GAMEPAD_BACK |
        XINPUT_GAMEPAD_LEFT_THUMB | XINPUT_GAMEPAD_RIGHT_THUMB |
        XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER |
        XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B |
        XINPUT_GAMEPAD_X | XINPUT_GAMEPAD_Y;

    uint8_t payload[33] = {};
    MapToDs5(NormalizeXInputState(gamepad), payload);
    const uint32_t buttons = static_cast<uint32_t>(payload[4]) |
        (static_cast<uint32_t>(payload[5]) << 8) |
        (static_cast<uint32_t>(payload[6]) << 16) |
        (static_cast<uint32_t>(payload[7]) << 24);
    const bool staleGatePass = !InputStreamNeedsNeutral(119999) &&
        !InputStreamNeedsNeutral(120000) &&
        InputStreamNeedsNeutral(120001) &&
        !InputStreamMustStop(1999999) &&
        !InputStreamMustStop(2000000) && InputStreamMustStop(2000001);

    vaderhaptics::input::FlydigiOnboardProfile expansion;
    expansion.controllerTargets.fill(-1);
    expansion.controllerTargets[2] = 4;  // M1 -> A / Cross
    expansion.controllerTargets[6] = 11; // LM -> RB / R1
    expansion.controllerTargets[8] = 12; // circle -> LT / L2
    NormalizedInputState expanded;
    expanded.vendorButtons = 0x44;
    expanded.vendorExtraButtons = 0x01;
    ApplyFlydigiExpansionKeys(expansion, expanded);
    uint8_t expandedPayload[33] = {};
    MapToDs5(expanded, expandedPayload);
    const uint32_t expandedButtons =
        static_cast<uint32_t>(expandedPayload[4]) |
        (static_cast<uint32_t>(expandedPayload[5]) << 8) |
        (static_cast<uint32_t>(expandedPayload[6]) << 16) |
        (static_cast<uint32_t>(expandedPayload[7]) << 24);
    const bool expansionPass = expandedButtons ==
            (kBtnCross | kBtnR1 | kBtnL2) &&
        expandedPayload[9] == 255;

    uint8_t renewalReply[33] = {0, 0x5A, 0xA5, 0x1C};
    VaderState packetState;
    bool commandReplyPass = !DecodeVaderReport(renewalReply, sizeof(renewalReply), packetState);
    renewalReply[3] = 0x01;
    commandReplyPass = commandReplyPass && !DecodeVaderReport(renewalReply, sizeof(renewalReply), packetState);
    renewalReply[3] = 0xEF;
    commandReplyPass = commandReplyPass && DecodeVaderReport(renewalReply, sizeof(renewalReply), packetState);

    const bool pass = static_cast<int8_t>(payload[0]) == -128 &&
        static_cast<int8_t>(payload[1]) == -128 &&
        static_cast<int8_t>(payload[2]) == 127 &&
        static_cast<int8_t>(payload[3]) == 127 &&
        buttons == 0x0002EFF0 && payload[8] == 0x09 &&
        payload[9] == 1 && payload[10] == 255 &&
        payload[15] == kTouchInactive && payload[20] == kTouchInactive &&
        Load16LE(payload + 31) == -8192 && staleGatePass && expansionPass && commandReplyPass;
    std::printf(
        "INPUT_MAPPING_SELFTEST result=%s lx=%d ly=%d rx=%d ry=%d "
        "buttons=0x%08X dpad=0x%02X l2=%u r2=%u expansion=%u\n",
        pass ? "PASS" : "FAIL",
        static_cast<int>(static_cast<int8_t>(payload[0])),
        static_cast<int>(static_cast<int8_t>(payload[1])),
        static_cast<int>(static_cast<int8_t>(payload[2])),
        static_cast<int>(static_cast<int8_t>(payload[3])),
        buttons, payload[8], payload[9], payload[10],
        expansionPass ? 1u : 0u);
    return pass ? 0 : 1;
}

bool ReadHidWithTimeout(HANDLE handle, uint8_t* data, DWORD size,
    DWORD timeoutMs, DWORD* bytesRead)
{
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        return false;
    }
    bool ok = false;
    bool completionObserved = false;
    DWORD transferred = 0;
    if (ReadFile(handle, data, size, nullptr, &overlapped)) {
        ok = true;
    }
    else if (GetLastError() == ERROR_IO_PENDING) {
        DWORD wait = WaitForSingleObject(overlapped.hEvent, timeoutMs);
        if (wait == WAIT_OBJECT_0) {
            ok = true;
        }
        else {
            // A cancelled overlapped request can still own both this event and
            // the caller's buffer. Complete it before either goes out of scope.
            CancelIoEx(handle, &overlapped);
            ok = GetOverlappedResult(
                handle, &overlapped, &transferred, TRUE) != 0;
            completionObserved = true;
        }
    }
    if (ok && !completionObserved) {
        ok = GetOverlappedResult(handle, &overlapped, &transferred, FALSE) != 0;
    }
    CloseHandle(overlapped.hEvent);
    if (bytesRead) {
        *bytesRead = transferred;
    }
    return ok;
}

bool WriteHidWithTimeout(HANDLE handle, const uint8_t* data, DWORD size)
{
    OVERLAPPED overlapped = {};
    overlapped.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!overlapped.hEvent) {
        return false;
    }
    bool ok = false;
    bool completionObserved = false;
    DWORD transferred = 0;
    if (WriteFile(handle, data, size, nullptr, &overlapped)) {
        ok = true;
    }
    else if (GetLastError() == ERROR_IO_PENDING) {
        if (WaitForSingleObject(overlapped.hEvent, 2000) == WAIT_OBJECT_0) {
            ok = true;
        }
        else {
            CancelIoEx(handle, &overlapped);
            ok = GetOverlappedResult(handle, &overlapped, &transferred, TRUE) != 0;
            completionObserved = true;
        }
    }
    if (ok && !completionObserved) {
        ok = GetOverlappedResult(handle, &overlapped, &transferred, FALSE) != 0;
    }
    CloseHandle(overlapped.hEvent);
    return ok && transferred == size;
}

bool SendAcquire(HANDLE handle, int outputLength, bool acquire)
{
    if (outputLength < 32) {
        return false;
    }
    std::vector<uint8_t> request(static_cast<size_t>(outputLength), 0);
    request[0] = 0x00;
    request[1] = 0x5A;
    request[2] = 0xA5;
    request[3] = 0x1C;
    request[4] = 23;
    request[5] = acquire ? 1 : 0;
    request[6] = 'S';
    request[7] = 'D';
    request[8] = 'L';
    return WriteHidWithTimeout(handle, request.data(), request.size());
}

bool SendInfoRequest(HANDLE handle, int outputLength)
{
    if (outputLength < 6) {
        return false;
    }
    std::vector<uint8_t> request(static_cast<size_t>(outputLength), 0);
    request[0] = 0x00;
    request[1] = 0x5A;
    request[2] = 0xA5;
    request[3] = 0x01;
    request[4] = 0x02;
    request[5] = 0x00;
    return WriteHidWithTimeout(handle, request.data(), request.size());
}

bool SendStatusRequest(HANDLE handle, int outputLength)
{
    if (outputLength < 4) {
        return false;
    }
    std::vector<uint8_t> request(static_cast<size_t>(outputLength), 0);
    request[0] = 0x00;
    request[1] = 0x5A;
    request[2] = 0xA5;
    request[3] = 0x10;
    return WriteHidWithTimeout(handle, request.data(), request.size());
}

class VaderAcquireHeartbeat
{
public:
    ~VaderAcquireHeartbeat() { Stop(); }

    bool Start(const HidDeviceInfo& device)
    {
        handle_ = CreateFileW(device.devicePath.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE) return false;
        stop_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!stop_) { CloseHandle(handle_); handle_ = INVALID_HANDLE_VALUE; return false; }
        const int outputLength = device.outputReportByteLength;
        worker_ = std::thread([this, outputLength] {
            // SDL Flydigi V2 renews ACQUIRE + GET_INFO every 30 seconds.
            // This maintenance worker never reads from the live input handle.
            while (WaitForSingleObject(stop_, 30000) == WAIT_TIMEOUT) {
                if (!SendAcquire(handle_, outputLength, true) ||
                    !SendInfoRequest(handle_, outputLength)) {
                    failed_ = true;
                    return;
                }
                ++renewals_;
            }
        });
        return true;
    }

    void Stop()
    {
        if (stop_) SetEvent(stop_);
        if (worker_.joinable()) worker_.join();
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
        if (stop_) CloseHandle(stop_);
        handle_ = INVALID_HANDLE_VALUE;
        stop_ = nullptr;
    }

    bool failed() const { return failed_.load(); }
    unsigned renewals() const { return renewals_.load(); }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    HANDLE stop_ = nullptr;
    std::thread worker_;
    std::atomic<bool> failed_{false};
    std::atomic<unsigned> renewals_{0};
};

bool SendReadOnlyFlydigiRequest(
    HANDLE handle, int outputLength, uint8_t command)
{
    if (outputLength < 4) return false;
    std::vector<uint8_t> request(static_cast<size_t>(outputLength), 0);
    request[0] = 0x00;
    request[1] = 0x5A;
    request[2] = 0xA5;
    request[3] = command;
    return WriteHidWithTimeout(handle, request.data(), request.size());
}

bool SendRawTransportRequest(HANDLE handle, int outputLength, bool enable)
{
    if (outputLength < 11) {
        return false;
    }
    std::vector<uint8_t> request(static_cast<size_t>(outputLength), 0);
    request[0] = 0x00;
    request[1] = 0x5A;
    request[2] = 0xA5;
    request[3] = 0x11;
    request[4] = 0x07;
    request[5] = 0xFF;
    request[6] = enable ? 0x01 : 0x00;
    request[7] = 0xFF;
    request[8] = 0xFF;
    request[9] = 0xFF;
    uint8_t checksum = 0;
    for (int i = 3; i <= 9; ++i) {
        checksum = static_cast<uint8_t>(checksum + request[i]);
    }
    request[10] = checksum;
    return WriteHidWithTimeout(handle, request.data(), request.size());
}

bool ReadFlydigiCommandReply(HANDLE handle, uint8_t command,
    uint8_t* reply, DWORD capacity, DWORD& replySize)
{
    for (int attempt = 0; attempt < 100; ++attempt) {
        std::memset(reply, 0, capacity);
        DWORD read = 0;
        if (!ReadHidWithTimeout(handle, reply, capacity, 20, &read)) {
            continue;
        }
        if (read > 1 && reply[0] != 0x5A && reply[1] == 0x5A) {
            std::memmove(reply, reply + 1, read - 1);
            --read;
        }
        if (read >= 3 && reply[0] == 0x5A && reply[1] == 0xA5 &&
            reply[2] == command) {
            replySize = read;
            return true;
        }
    }
    replySize = 0;
    return false;
}

struct FlydigiOnboardIdentity
{
    std::string uid;
    uint8_t activeSlot = 0;
};

bool QueryFlydigiOnboardIdentity(HANDLE handle, int outputLength,
    FlydigiOnboardIdentity& identity)
{
    uint8_t reply[64] = {};
    DWORD replySize = 0;
    if (!SendReadOnlyFlydigiRequest(handle, outputLength, 0x04) ||
        !ReadFlydigiCommandReply(
            handle, 0x04, reply, sizeof(reply), replySize) ||
        replySize < 18) {
        std::printf("expansion profile: controller UID read unavailable\n");
        return false;
    }
    char uid[27] = {};
    for (std::size_t index = 0; index < 13; ++index) {
        std::snprintf(uid + index * 2, 3, "%02x", reply[5 + index]);
    }

    if (!SendReadOnlyFlydigiRequest(handle, outputLength, 0xA1) ||
        !ReadFlydigiCommandReply(
            handle, 0xA1, reply, sizeof(reply), replySize) ||
        replySize < 6 || reply[5] > 3) {
        std::printf("expansion profile: active onboard slot read unavailable\n");
        return false;
    }
    identity.uid = uid;
    identity.activeSlot = reply[5];
    return true;
}

bool LoadFlydigiExpansionProfile(HANDLE handle, int outputLength,
    const std::string& onboardDirectory,
    vaderhaptics::input::FlydigiOnboardProfile& profile)
{
    if (onboardDirectory.empty()) {
        std::printf("expansion profile: disabled (no onboard directory)\n");
        return false;
    }
    FlydigiOnboardIdentity identity;
    if (!QueryFlydigiOnboardIdentity(handle, outputLength, identity)) {
        return false;
    }

    const std::filesystem::path path =
        std::filesystem::path(onboardDirectory) / (identity.uid + ".dat");
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        std::printf("expansion profile: onboard file unavailable: %s\n",
            path.string().c_str());
        return false;
    }
    const std::vector<uint8_t> bytes{
        std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>()};
    std::string error;
    if (!vaderhaptics::input::ParseFlydigiOnboardProfile(
            bytes, identity.activeSlot, profile, error)) {
        std::printf("expansion profile: ignored: %s\n", error.c_str());
        return false;
    }

    std::printf("expansion profile: PASS uid=%s slot=%u name=%s "
                "keyboard_macro_ignored=%u unsupported_ignored=%u\n",
        identity.uid.c_str(), profile.slot, profile.name.c_str(),
        profile.ignoredNonControllerMappings,
        profile.ignoredUnsupportedTargets);
    static constexpr const char* kSourceNames[] = {
        "C", "Z", "M1", "M2", "M3", "M4", "LM", "RM", "Circle"};
    for (std::size_t index = 0; index < profile.controllerTargets.size();
         ++index) {
        if (profile.controllerTargets[index] < 0) continue;
        std::printf("  expansion map %s -> %s\n", kSourceNames[index],
            FlydigiControllerKeyName(profile.controllerTargets[index]));
    }
    return true;
}

bool ReadLiveVaderState(HANDLE handle, VaderState& state)
{
    for (int attempt = 0; attempt < 250; ++attempt) {
        uint8_t report[64] = {};
        DWORD read = 0;
        if (ReadHidWithTimeout(handle, report, sizeof(report), 10, &read) &&
            read > 0 && DecodeVaderReport(report, read, state)) {
            return true;
        }
    }
    return false;
}

bool ValidateAndOpenVaderCandidate(const HidDeviceInfo& device,
    size_t candidateIndex, HANDLE& handle, VaderState& firstState)
{
    std::printf("Vader MI_01 candidate[%zu]: path=%ls inLen=%u outLen=%u\n",
        candidateIndex, device.devicePath.c_str(),
        device.inputReportByteLength, device.outputReportByteLength);

    handle = CreateFileW(device.devicePath.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::printf("  reject: open failed (error %lu)\n", GetLastError());
        return false;
    }

    bool rawTransportChanged = false;
    auto reject = [&](const char* reason) {
        std::printf("  reject: %s\n", reason);
        uint8_t releaseReply[64] = {};
        DWORD releaseSize = 0;
        if (SendAcquire(handle, device.outputReportByteLength, false) &&
            ReadFlydigiCommandReply(handle, 0x1C, releaseReply,
                sizeof(releaseReply), releaseSize)) {
            std::printf("  rollback: controller acquire released\n");
        }
        else {
            std::printf("  WARNING: controller acquire rollback was not acknowledged\n");
        }
        if (rawTransportChanged) {
            uint8_t rollbackReply[64] = {};
            DWORD rollbackSize = 0;
            if (SendRawTransportRequest(
                    handle, device.outputReportByteLength, false) &&
                ReadFlydigiCommandReply(handle, 0x11, rollbackReply,
                    sizeof(rollbackReply), rollbackSize)) {
                std::printf("  rollback: raw transport restored to disabled\n");
            }
            else {
                std::printf("  WARNING: raw transport rollback was not acknowledged\n");
            }
        }
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
        return false;
    };

    uint8_t reply[64] = {};
    DWORD replySize = 0;
    if (!SendInfoRequest(handle, device.outputReportByteLength) ||
        !ReadFlydigiCommandReply(
            handle, 0x01, reply, sizeof(reply), replySize)) {
        return reject("GET_INFO command reply missing");
    }
    if (replySize <= 16 || reply[5] != 130) {
        return reject("GET_INFO did not identify Vader 5 Pro deviceID 130");
    }
    const uint16_t firmware = static_cast<uint16_t>(
        (static_cast<uint16_t>(reply[15]) << 8) | reply[16]);
    std::printf("  GET_INFO ok: deviceID=%u firmware=0x%04X connection=%u\n",
        reply[5], firmware, reply[6]);

    if (!SendStatusRequest(handle, device.outputReportByteLength) ||
        !ReadFlydigiCommandReply(
            handle, 0x10, reply, sizeof(reply), replySize) ||
        replySize <= 9) {
        return reject("GET_STATUS command reply missing or short");
    }
    std::printf("  GET_STATUS ok: rawTransport=%u thirdPartyAllowed=%u\n",
        reply[6], reply[9]);

    if (reply[6] != 1) {
        if (!SendRawTransportRequest(
                handle, device.outputReportByteLength, true) ||
            !ReadFlydigiCommandReply(
                handle, 0x11, reply, sizeof(reply), replySize)) {
            return reject("raw transport enable command was not acknowledged");
        }
        rawTransportChanged = true;
        if (!SendStatusRequest(handle, device.outputReportByteLength) ||
            !ReadFlydigiCommandReply(
                handle, 0x10, reply, sizeof(reply), replySize) ||
            replySize <= 9 || reply[6] != 1) {
            return reject("raw transport enable did not survive status readback");
        }
        std::printf("  raw transport enabled and confirmed by GET_STATUS\n");
    }
    else {
        std::printf("  raw transport already enabled\n");
    }

    if (!SendAcquire(handle, device.outputReportByteLength, true) ||
        !ReadFlydigiCommandReply(
            handle, 0x1C, reply, sizeof(reply), replySize) ||
        replySize <= 6 || reply[5] != 1) {
        return reject("ACQUIRE command reply missing or unsuccessful");
    }
    std::printf("  ACQUIRE ok: reply[5]=%u reply[6]=%u\n",
        reply[5], reply[6]);

    if (!ReadLiveVaderState(handle, firstState)) {
        return reject("no decodable 0xEF state stream after ACQUIRE");
    }
    std::printf("  selected: live 0xEF stream confirmed (LX=%d)\n",
        firstState.lx);
    return true;
}

bool OpenLiveVaderMi01(HidDeviceInfo& selected, HANDLE& handle,
    VaderState& firstState)
{
    const auto devices = EnumerateHidDevices();
    std::vector<HidDeviceInfo> candidates;
    for (const auto& device : devices) {
        if (IsVader5Pro(device) && device.usagePage == 0xFFA0) {
            candidates.push_back(device);
        }
    }
    std::printf("Vader MI_01 0xFFA0 candidates: %zu\n", candidates.size());
    for (size_t index = 0; index < candidates.size(); ++index) {
        HANDLE candidateHandle = INVALID_HANDLE_VALUE;
        VaderState candidateState;
        if (ValidateAndOpenVaderCandidate(candidates[index], index,
                candidateHandle, candidateState)) {
            selected = candidates[index];
            handle = candidateHandle;
            firstState = candidateState;
            return true;
        }
    }
    handle = INVALID_HANDLE_VALUE;
    return false;
}

bool ReleaseVaderOwnership()
{
    const auto devices = EnumerateHidDevices();
    size_t candidateIndex = 0;
    for (const auto& device : devices) {
        if (!IsVader5Pro(device) || device.usagePage != 0xFFA0) {
            continue;
        }

        HANDLE handle = CreateFileW(device.devicePath.c_str(),
            GENERIC_READ | GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
            FILE_FLAG_OVERLAPPED, nullptr);
        if (handle == INVALID_HANDLE_VALUE) {
            ++candidateIndex;
            continue;
        }

        uint8_t reply[64] = {};
        DWORD replySize = 0;
        const bool identified =
            SendInfoRequest(handle, device.outputReportByteLength) &&
            ReadFlydigiCommandReply(
                handle, 0x01, reply, sizeof(reply), replySize) &&
            replySize > 16 && reply[5] == 130;
        if (!identified) {
            CloseHandle(handle);
            ++candidateIndex;
            continue;
        }

        uint8_t zero[33] = {};
        vaderhaptics::m3::BuildVaderHapticPacket(0, 0, 0, 0, zero);
        const bool zeroWritten = WriteHidWithTimeout(
            handle, zero, static_cast<DWORD>(sizeof(zero)));

        replySize = 0;
        const bool acquireReleased =
            SendAcquire(handle, device.outputReportByteLength, false) &&
            ReadFlydigiCommandReply(
                handle, 0x1C, reply, sizeof(reply), replySize);

        replySize = 0;
        const bool rawDisabled =
            SendRawTransportRequest(
                handle, device.outputReportByteLength, false) &&
            ReadFlydigiCommandReply(
                handle, 0x11, reply, sizeof(reply), replySize);

        replySize = 0;
        const bool statusRead =
            SendStatusRequest(handle, device.outputReportByteLength) &&
            ReadFlydigiCommandReply(
                handle, 0x10, reply, sizeof(reply), replySize) &&
            replySize > 9;
        const unsigned rawTransport = statusRead ? reply[6] : 0xFF;
        const unsigned takeoverAllowed = statusRead ? reply[9] : 0xFF;
        CloseHandle(handle);

        const bool pass = zeroWritten && acquireReleased && rawDisabled &&
            statusRead && rawTransport == 0;
        std::printf("VADER_OWNERSHIP_RELEASE result=%s candidate=%zu "
                    "zero=%u acquire_off=%u raw_off=%u rawTransport=%u "
                    "thirdPartyAllowed=%u\n",
            pass ? "PASS" : "FAIL", candidateIndex,
            zeroWritten ? 1u : 0u, acquireReleased ? 1u : 0u,
            rawDisabled ? 1u : 0u, rawTransport, takeoverAllowed);
        return pass;
    }

    std::printf("VADER_OWNERSHIP_RELEASE result=FAIL reason=no-accessible-vader-mi01\n");
    return false;
}

class VaderOwnershipGuard
{
public:
    VaderOwnershipGuard() = default;
    VaderOwnershipGuard(const VaderOwnershipGuard&) = delete;
    VaderOwnershipGuard& operator=(const VaderOwnershipGuard&) = delete;

    ~VaderOwnershipGuard()
    {
        if (armed_) {
            ReleaseVaderOwnership();
        }
    }

    bool ReleaseNow()
    {
        if (!armed_) {
            return true;
        }
        armed_ = false;
        return ReleaseVaderOwnership();
    }

private:
    bool armed_ = true;
};

HANDLE OpenSeparateVaderOutputHandle(const HidDeviceInfo& device)
{
    HANDLE handle = CreateFileW(device.devicePath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_OVERLAPPED, nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        std::printf("MI_01 output handle: separate open failed (error %lu); "
                    "falling back to shared read/write handle\n", GetLastError());
    }
    else {
        std::printf("MI_01 output handle: separate write handle active\n");
    }
    return handle;
}

bool PingViiper(std::string& version, int timeoutMs = 3000)
{
    std::string response;
    if (!ApiRequest("ping", response, timeoutMs)) {
        return false;
    }
    version = FindJsonValue(response, "version");
    return !response.empty();
}

std::vector<char> ViiperEnvironment()
{
    // Configure only the child. GUI builds can inject their own `server`
    // command; upstream's documented environment options survive that path.
    std::vector<std::string> entries;
    LPCH environment = GetEnvironmentStringsA();
    if (environment) {
        for (const char* entry = environment; *entry; entry += std::strlen(entry) + 1) {
            if (_strnicmp(entry, "VIIPER_USB_ADDR=", 16) == 0 ||
                _strnicmp(entry, "VIIPER_API_ADDR=", 16) == 0) continue;
            entries.emplace_back(entry);
        }
        FreeEnvironmentStringsA(environment);
    }
    for (const auto& entry : {
        "VIIPER_USB_ADDR=127.0.0.1:" + std::to_string(kUsbPort),
        "VIIPER_API_ADDR=127.0.0.1:" + std::to_string(kApiPort)}) {
        entries.push_back(entry);
    }
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return _stricmp(a.c_str(), b.c_str()) < 0;
    });
    std::vector<char> result;
    for (const auto& entry : entries) {
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back('\0');
    }
    result.push_back('\0');
    return result;
}

bool StartViiperServer(DWORD& pid, const std::string& logPath =
    ExpandEnv("%TEMP%\\viiper_server_bridge.log"))
{
    std::string exe = g_viiperExe;
    std::string args = "\"" + exe + "\" server";
    SECURITY_ATTRIBUTES security = {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE logFile = CreateFileA(logPath.c_str(), GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE input = CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_EXISTING, 0, nullptr);
    if (logFile == INVALID_HANDLE_VALUE || input == INVALID_HANDLE_VALUE) {
        std::printf("ERROR: VIIPER log setup failed (Windows error %lu)\n", GetLastError());
        if (logFile != INVALID_HANDLE_VALUE) CloseHandle(logFile);
        if (input != INVALID_HANDLE_VALUE) CloseHandle(input);
        return false;
    }
    std::printf("VIIPER startup log: %s\n", logPath.c_str());
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    if (logFile != INVALID_HANDLE_VALUE) {
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdOutput = logFile;
        si.hStdError = logFile;
        si.hStdInput = input;
    }
    PROCESS_INFORMATION pi = {};
    auto environment = ViiperEnvironment();
    std::printf("VIIPER endpoints: usb=127.0.0.1:%d api=127.0.0.1:%d\n", kUsbPort, kApiPort);
    if (!CreateProcessA(exe.c_str(), args.data(), nullptr, nullptr, TRUE,
            CREATE_NO_WINDOW, environment.data(), nullptr, &si, &pi)) {
        std::printf("ERROR: VIIPER process launch failed (Windows error %lu)\n", GetLastError());
        CloseHandle(input);
        if (logFile != INVALID_HANDLE_VALUE) {
            CloseHandle(logFile);
        }
        return false;
    }
    CloseHandle(input);
    if (logFile != INVALID_HANDLE_VALUE) {
        CloseHandle(logFile);
    }
    CloseHandle(pi.hThread);
    pid = pi.dwProcessId;

    const auto deadline = GetTickCount64() + 8000;
    while (GetTickCount64() < deadline) {
        if (WaitForSingleObject(pi.hProcess, 100) == WAIT_OBJECT_0) {
            DWORD exitCode = 0;
            GetExitCodeProcess(pi.hProcess, &exitCode);
            std::printf("ERROR: VIIPER exited before ready (exit code %lu); see %s\n",
                exitCode, logPath.c_str());
            CloseHandle(pi.hProcess);
            return false;
        }
        if (g_bridgeStopEvent && WaitForSingleObject(g_bridgeStopEvent, 0) == WAIT_OBJECT_0) break;
        std::string version;
        if (PingViiper(version, 300) && WaitForSingleObject(pi.hProcess, 0) == WAIT_TIMEOUT) {
            CloseHandle(pi.hProcess);
            return true;
        }
    }
    std::printf("ERROR: VIIPER startup timed out or was cancelled; see %s\n", logPath.c_str());
    TerminateProcess(pi.hProcess, 1);
    WaitForSingleObject(pi.hProcess, 3000);
    CloseHandle(pi.hProcess);
    return false;
}

int RunViiperStartupSelfTest()
{
    // This executable has no `server` command: it prints usage and exits.
    // Exercise real child-exit detection and stdout inheritance without VIIPER,
    // controller access, driver operations or a virtual device.
    char executable[32768] = {};
    GetModuleFileNameA(nullptr, executable, static_cast<DWORD>(std::size(executable)));
    g_viiperExe = executable;
    const auto logPath = ExpandEnv("%TEMP%\\vader_viiper_startup_test_")
        + std::to_string(GetCurrentProcessId()) + ".log";
    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
    DWORD pid = 0;
    const auto start = GetTickCount64();
    const bool started = StartViiperServer(pid, logPath);
    const auto elapsed = GetTickCount64() - start;
    WSACleanup();
    std::ifstream input(logPath);
    const std::string log{std::istreambuf_iterator<char>(input), {}};
    input.close();
    DeleteFileA(logPath.c_str());
    const auto environment = ViiperEnvironment();
    const std::string environmentText(environment.data(), environment.size());
    const bool passed = !started && elapsed < 2500 && log.find("usage:") != std::string::npos
        && environmentText.find("VIIPER_USB_ADDR=127.0.0.1:43241") != std::string::npos
        && environmentText.find("VIIPER_API_ADDR=127.0.0.1:43242") != std::string::npos;
    std::printf("VIIPER_STARTUP_SELFTEST result=%s elapsed_ms=%llu captured_output=%u\n",
        passed ? "PASS" : "FAIL", elapsed, log.empty() ? 0u : 1u);
    return passed ? 0 : 1;
}

int RunUsbipDetach(int port)
{
    std::string args = "\"C:\\Program Files\\USBip\\usbip.exe\" detach --port=" +
        std::to_string(port);
    STARTUPINFOA si = {};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi = {};
    if (!CreateProcessA("C:\\Program Files\\USBip\\usbip.exe",
            args.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
            nullptr, &si, &pi)) {
        return -1;
    }
    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    return static_cast<int>(code);
}

void PrintVirtualDualSense()
{
    auto devices = EnumerateHidDevices();
    int count = 0;
    for (const auto& device : devices) {
        if (device.vid == 0x054C && device.pid == 0x0CE6) {
            std::wprintf(L"  virtual DS5: %s\n", device.devicePath.c_str());
            std::printf("    usagePage=0x%04X usage=0x%04X inLen=%u\n",
                device.usagePage, device.usage,
                device.inputReportByteLength);
            ++count;
        }
    }
    std::printf("virtual DualSense HID interfaces present: %d\n", count);
}

int CountVirtualDualSenseHidInterfaces()
{
    int count = 0;
    for (const auto& device : EnumerateHidDevices()) {
        if (device.vid == 0x054C && device.pid == 0x0CE6) {
            ++count;
        }
    }
    return count;
}

int CountConnectedXInputSlots()
{
    int count = 0;
    for (DWORD slot = 0; slot < XUSER_MAX_COUNT; ++slot) {
        XINPUT_STATE state = {};
        if (XInputGetState(slot, &state) == ERROR_SUCCESS) {
            ++count;
        }
    }
    return count;
}

int QueryFreshProcessXInputCount()
{
    char executable[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, executable, MAX_PATH) == 0) {
        return -1;
    }
    std::string commandLine = std::string("\"") + executable +
        "\" xinput-count";
    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process = {};
    if (!CreateProcessA(executable, commandLine.data(),
            nullptr, nullptr,
            FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process)) {
        return -1;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 5000);
    DWORD exitCode = 255;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    else {
        TerminateProcess(process.hProcess, 1);
    }
    CloseHandle(process.hProcess);
    return exitCode <= XUSER_MAX_COUNT ? static_cast<int>(exitCode) : -1;
}

bool WaitForFreshProcessXInputCount(int expected, DWORD timeoutMs = 5000)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do {
        if (QueryFreshProcessXInputCount() == expected) {
            return true;
        }
        if (GetTickCount64() >= deadline) {
            break;
        }
        Sleep(100);
    } while (true);
    return false;
}

void PrintVirtualDualSenseBounded()
{
    std::atomic<bool> done{false};
    std::thread worker([&]() {
        PrintVirtualDualSense();
        done = true;
    });
    for (int i = 0; i < 40 && !done.load(); ++i) {
        Sleep(100);
    }
    if (!done.load()) {
        std::printf("virtual DualSense enumeration timed out; skipped\n");
        worker.detach();
    }
    else {
        worker.join();
    }
}

using BridgeState = vaderhaptics::runtime::SessionState;

bool WriteStateFile(const BridgeState& state)
{
    std::string error;
    if (!vaderhaptics::runtime::WriteSessionState(
            GetStatePath(), state, error)) {
        std::printf("ERROR: %s\n", error.c_str());
        return false;
    }
    return true;
}

vaderhaptics::runtime::SessionStateReadResult ReadStateFile(
    BridgeState& state)
{
    std::string error;
    const auto result = vaderhaptics::runtime::ReadSessionState(
        GetStatePath(), state, error);
    if (result == vaderhaptics::runtime::SessionStateReadResult::Invalid) {
        std::printf("ERROR: invalid bridge recovery state at %s: %s\n",
            GetStatePath().c_str(), error.c_str());
    }
    return result;
}

bool RestorePhysicalGamePersona(BridgeState& state)
{
    if (!state.physicalPersonaRestorePending ||
        state.physicalGamePersonaId.empty()) {
        return true;
    }
    const std::wstring instance = WidenAscii(state.physicalGamePersonaId);
    if (!SetDeviceStarted(instance, true)) {
        std::printf("ERROR: physical game persona restoration remains pending; "
                    "state file retained at %s\n", GetStatePath().c_str());
        return false;
    }
    state.physicalPersonaRestorePending = false;
    WriteStateFile(state);
    return true;
}

int VisibilityApply(const std::vector<std::string>& paths);
int VisibilityRestore();
bool VerifyNativeVisibilityBaseline();

bool CleanupBridge(BridgeState& state)
{
    if (!state.busId.empty() && !state.devId.empty()) {
        std::string response;
        ApiRequest("bus/" + state.busId + "/remove " + state.devId, response);
        ApiRequest("bus/remove " + state.busId, response);
    }
    if (state.usbipPort >= 0) {
        int code = RunUsbipDetach(state.usbipPort);
        std::printf("usbip detach port %d: exit %d\n", state.usbipPort, code);
    }
    if (state.startedServer && state.serverPid != 0) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE, FALSE,
            static_cast<DWORD>(state.serverPid));
        if (process) {
            TerminateProcess(process, 0);
            CloseHandle(process);
            std::printf("stopped VIIPER server pid %lu\n",
                static_cast<unsigned long>(state.serverPid));
        }
    }
    const bool visibilityRestored = VisibilityRestore() == 0;
    const bool pnpRestored = RestorePhysicalGamePersona(state);
    const bool restored = visibilityRestored && pnpRestored;
    if (restored) {
        DeleteFileA(GetStatePath().c_str());
    }
    return restored;
}

bool EnterPhysicalGamePersonaIsolation(BridgeState& state)
{
    (void)state;
    const auto parents = ResolveCurrentVaderMi00Instances(true);
    if (parents.size() != 1) {
        std::printf("ERROR: expected exactly one present Vader 37D7:2401 "
                    "MI_00 parent; found %zu\n", parents.size());
        for (const auto& parent : parents) {
            std::printf("  candidate: %s\n", NarrowAscii(parent).c_str());
        }
        return false;
    }
    bool started = false;
    if (!IsDeviceStarted(parents.front(), started) || !started) {
        std::printf("ERROR: resolved Vader MI_00 is not started; refusing to "
                    "claim restoration ownership\n");
        return false;
    }
    const auto gameChildren = ResolveDirectGameChildren(parents.front());
    if (gameChildren.size() != 1) {
        std::printf("ERROR: expected exactly one direct Vader IG game child "
                    "under the current MI_00; found %zu\n",
            gameChildren.size());
        for (const auto& child : gameChildren) {
            std::printf("  candidate: %s\n", NarrowAscii(child).c_str());
        }
        return false;
    }
    if (!IsDeviceStarted(gameChildren.front(), started) || !started) {
        std::printf("ERROR: resolved Vader IG game child is not started; "
                    "refusing to claim restoration ownership\n");
        return false;
    }
    const auto gameHids = ResolveDirectGameHidChildren(gameChildren.front());
    if (gameHids.size() != 1) {
        std::printf("ERROR: expected exactly one direct Vader game HID under "
                    "the current IG child; found %zu\n", gameHids.size());
        for (const auto& gameHid : gameHids) {
            std::printf("  candidate: %s\n", NarrowAscii(gameHid).c_str());
        }
        return false;
    }
    if (!IsDeviceStarted(gameHids.front(), started) || !started) {
        std::printf("ERROR: resolved Vader game HID is not started; refusing "
                    "to claim visibility ownership\n");
        return false;
    }
    const int xinputBefore = QueryFreshProcessXInputCount();
    if (xinputBefore != 1) {
        std::printf("ERROR: child-isolation gate requires exactly one "
                    "connected XInput slot before transition; found %d\n",
            xinputBefore);
        return false;
    }
    std::vector<std::string> targets = {
        NarrowAscii(parents.front()), NarrowAscii(gameHids.front())};
    if (VisibilityApply(targets) != 0) {
        VisibilityRestore();
        return false;
    }
    if (!WaitForFreshProcessXInputCount(0)) {
        std::printf("ERROR: a fresh non-whitelisted process still saw "
                    "physical XInput after applying the dynamic HidHide "
                    "targets; restoring visibility\n");
        VisibilityRestore();
        return false;
    }
    std::printf("DualSense topology: physical XUSB=%s and game HID=%s hidden; "
                "XInput isolation confirmed; MI_01 will be reopened and "
                "revalidated\n", targets[0].c_str(), targets[1].c_str());
    return true;
}

bool EnsureViiperServer(BridgeState& state)
{
    std::string version;
    if (PingViiper(version)) {
        std::printf("VIIPER server already running (%s)\n", version.c_str());
        return true;
    }
    std::printf("VIIPER server not running; starting it...\n");
    DWORD serverPid = 0;
    if (!StartViiperServer(serverPid)) {
        std::printf("ERROR: could not start VIIPER server\n");
        return false;
    }
    state.serverPid = static_cast<uint32_t>(serverPid);
    state.startedServer = true;
    std::printf("VIIPER server started (pid %lu)\n",
        static_cast<unsigned long>(state.serverPid));
    return true;
}

bool CreatePersona(BridgeState& state, SOCKET& stream)
{
    std::string response;
    if (!ApiRequest("bus/create", response)) {
        std::printf("ERROR: bus/create failed\n");
        return false;
    }
    state.busId = FindJsonValue(response, "busId");
    std::printf("bus/create -> %s\n", response.c_str());

    std::string add = "bus/" + state.busId + "/add {\"type\":\"" +
        kDeviceType + "\"}";
    if (!ApiRequest(add, response)) {
        std::printf("ERROR: bus/add failed\n");
        return false;
    }
    state.devId = FindJsonValue(response, "devId");
    std::string portStr = FindJsonValue(response, "usbipPort");
    state.usbipPort = portStr.empty() ? -1 : std::atoi(portStr.c_str());
    std::printf("device add -> %s\n", response.c_str());
    if (FindJsonValue(response, "vaderhapticsInputRevision") != "1" ||
        FindJsonValue(response, "inputQueueCapacity") != "8") {
        std::printf("ERROR: VIIPER input queue fix is missing; install the supported runtime build before playing\n");
        return false;
    }
    std::printf("VIIPER input queue compatibility: PASS revision=1 capacity=8\n");

    stream = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kApiPort);
    inet_pton(AF_INET, kApiHost, &addr.sin_addr);
    if (stream == INVALID_SOCKET ||
        connect(stream, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        std::printf("ERROR: stream connect failed\n");
        return false;
    }
    DWORD streamTimeout = 2000;
    setsockopt(stream, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char*>(&streamTimeout), sizeof(streamTimeout));
    setsockopt(stream, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&streamTimeout), sizeof(streamTimeout));
    std::string handshake = "bus/" + state.busId + "/" + state.devId;
    handshake.push_back('\0');
    send(stream, handshake.data(), static_cast<int>(handshake.size()), 0);

    WriteStateFile(state);
    std::printf("persona active: bus=%s dev=%s usbipPort=%d\n",
        state.busId.c_str(), state.devId.c_str(), state.usbipPort);
    return true;
}

bool DestroyPersona(BridgeState& state, SOCKET stream)
{
    const bool restored = CleanupBridge(state);
    if (stream != INVALID_SOCKET) {
        closesocket(stream);
    }
    return restored;
}

struct M3LiveTelemetry {
    uint64_t blocksReceived84 = 0;
    uint64_t blocksDecoded = 0;
    uint64_t decodeFailures = 0;
    uint64_t activeBlocks = 0;
    uint64_t zeroBlocks = 0;
    uint64_t rendererProcessed = 0;
    uint64_t generationResets = 0;
    uint64_t watchdogZeroEvents = 0;
    uint64_t enqueued = 0;
    uint64_t written = 0;
    uint64_t droppedStale = 0;
    uint64_t coalesced = 0;
    uint64_t writeFailures = 0;
    uint64_t suppressedRedundantZero = 0;
    uint64_t suppressedRedundantState = 0;
    uint64_t maxQueueDepth = 0;
    int64_t last0x84Us = 0;
    double leftGripMax = 0.0;
    double leftGripMeanSum = 0.0;
    double rightGripMax = 0.0;
    double rightGripMeanSum = 0.0;
    double leftTriggerMax = 0.0;
    double rightTriggerMax = 0.0;
    uint64_t nonzeroBlocks = 0;
    uint64_t capBlocks = 0;
    std::vector<uint64_t> processingNs;
    bool runtimeFailed = false;
};

double Percentile(const std::vector<uint64_t>& values, double quantile)
{
    if (values.empty()) {
        return 0.0;
    }
    std::vector<uint64_t> sorted = values;
    std::sort(sorted.begin(), sorted.end());
    const size_t index = static_cast<size_t>(
        quantile * static_cast<double>(sorted.size() - 1));
    return static_cast<double>(sorted[index]);
}

std::string ResolveM3ConfigPath(const std::string& explicitPath)
{
    if (!explicitPath.empty()) {
        return explicitPath;
    }
    const char* envPath = std::getenv("VADERHAPTICS_M3_CONFIG");
    if (envPath != nullptr && envPath[0] != '\0') {
        return envPath;
    }
    return "config/m3_calibration.json";
}

enum class InputSourceKind
{
    Mi01,
    XInput,
};

enum class InputPollResult
{
    Sample,
    NoSample,
    Disconnected,
};

class OrdinaryInputSource
{
public:
    virtual ~OrdinaryInputSource() = default;
    virtual const char* name() const = 0;
    virtual int deviceIndex() const = 0;
    virtual InputPollResult Poll(NormalizedInputState& state) = 0;
};

class Mi01OrdinaryInputSource final : public OrdinaryInputSource
{
public:
    Mi01OrdinaryInputSource(HANDLE handle, const VaderState& initial,
        const vaderhaptics::input::FlydigiOnboardProfile* expansionProfile)
        : initial_(initial)
    {
        if (!DuplicateHandle(GetCurrentProcess(), handle, GetCurrentProcess(),
                &handle_, 0, FALSE, DUPLICATE_SAME_ACCESS)) {
            handle_ = INVALID_HANDLE_VALUE;
        }
        read_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (expansionProfile) {
            expansionProfile_ = *expansionProfile;
            expansionMappingEnabled_ = true;
        }
    }

    ~Mi01OrdinaryInputSource() override
    {
        if (pending_) {
            CancelIoEx(handle_, &read_);
            DWORD ignored = 0;
            GetOverlappedResult(handle_, &read_, &ignored, TRUE);
        }
        if (read_.hEvent) CloseHandle(read_.hEvent);
        if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_);
    }

    const char* name() const override
    {
        return "mi01";
    }

    int deviceIndex() const override
    {
        return -1;
    }

    InputPollResult Poll(NormalizedInputState& state) override
    {
        if (handle_ == INVALID_HANDLE_VALUE || !read_.hEvent) return InputPollResult::Disconnected;
        if (initialPending_) {
            initialPending_ = false;
            state = NormalizeMi01State(initial_);
            if (expansionMappingEnabled_) {
                ApplyFlydigiExpansionKeys(expansionProfile_, state);
            }
            return InputPollResult::Sample;
        }
        if (!pending_) {
            ResetEvent(read_.hEvent);
            if (!ReadFile(handle_, report_.data(), static_cast<DWORD>(report_.size()), nullptr, &read_)
                && GetLastError() != ERROR_IO_PENDING) {
                return InputPollResult::Disconnected;
            }
            pending_ = true;
        }
        // Keep the same read alive across short polling timeouts. Cancelling
        // every 4 ms needlessly restarts the pending transfer under load.
        if (WaitForSingleObject(read_.hEvent, 4) == WAIT_TIMEOUT) {
            return InputPollResult::NoSample;
        }
        DWORD bytesRead = 0;
        if (!GetOverlappedResult(handle_, &read_, &bytesRead, FALSE)) {
            if (GetLastError() == ERROR_IO_INCOMPLETE) return InputPollResult::NoSample;
            pending_ = false;
            return InputPollResult::Disconnected;
        }
        pending_ = false;
        VaderState decoded;
        if (!DecodeVaderReport(report_.data(), bytesRead, decoded)) {
            return InputPollResult::NoSample;
        }
        state = NormalizeMi01State(decoded);
        if (expansionMappingEnabled_) {
            ApplyFlydigiExpansionKeys(expansionProfile_, state);
        }
        return InputPollResult::Sample;
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    OVERLAPPED read_ = {};
    std::array<uint8_t, 64> report_{};
    bool pending_ = false;
    VaderState initial_;
    bool initialPending_ = true;
    vaderhaptics::input::FlydigiOnboardProfile expansionProfile_;
    bool expansionMappingEnabled_ = false;
};

int RunMi01ReadSelfTest()
{
    const std::wstring name = L"\\\\.\\pipe\\VaderHapticsInputTest-" + std::to_wstring(GetCurrentProcessId());
    HANDLE pipe = CreateNamedPipeW(name.c_str(), PIPE_ACCESS_INBOUND | FILE_FLAG_OVERLAPPED,
        PIPE_TYPE_MESSAGE | PIPE_READMODE_MESSAGE | PIPE_WAIT, 1, 64, 64, 0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) return 1;
    HANDLE writer = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (writer == INVALID_HANDLE_VALUE) { CloseHandle(pipe); return 1; }
    OVERLAPPED connection = {};
    connection.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    bool connected = ConnectNamedPipe(pipe, &connection) != FALSE;
    if (!connected) connected = GetLastError() == ERROR_PIPE_CONNECTED;
    if (connection.hEvent) CloseHandle(connection.hEvent);
    bool pass = connected;
    if (pass) {
        VaderState initial;
        Mi01OrdinaryInputSource source(pipe, initial, nullptr);
        NormalizedInputState state;
        pass = source.Poll(state) == InputPollResult::Sample;
        // Let several poll windows expire before completing the same OS read.
        for (int i = 0; i < 8; ++i) pass = (source.Poll(state) == InputPollResult::NoSample) && pass;
        uint8_t report[33] = {0, 0x5A, 0xA5, 0xEF};
        report[12] = 0x10; // physical A -> Cross, held throughout all samples
        auto readButtons = [&]() {
            uint8_t payload[33] = {};
            MapToDs5(state, payload);
            const auto frame = BuildVpcmInputFrame(1, payload);
            return static_cast<uint32_t>(frame[20]) | (static_cast<uint32_t>(frame[21]) << 8)
                | (static_cast<uint32_t>(frame[22]) << 16) | (static_cast<uint32_t>(frame[23]) << 24);
        };
        for (int i = 0; i < 5000; ++i) {
            DWORD written = 0;
            pass = WriteFile(writer, report, sizeof(report), &written, nullptr) && written == sizeof(report) && pass;
            pass = (source.Poll(state) == InputPollResult::Sample && readButtons() == kBtnCross) && pass;
        }
        report[12] = 0;
        DWORD written = 0;
        pass = WriteFile(writer, report, sizeof(report), &written, nullptr) && pass;
        pass = (source.Poll(state) == InputPollResult::Sample && !state.a) && pass;
        for (int tap = 0; tap < 1000; ++tap) {
            for (const auto button : {uint8_t{0x10}, uint8_t{0}}) {
                report[12] = button;
                pass = WriteFile(writer, report, sizeof(report), &written, nullptr) && pass;
                pass = (source.Poll(state) == InputPollResult::Sample
                    && readButtons() == (button ? kBtnCross : 0)) && pass;
            }
        }
        report[12] = 0x50; // A + View
        report[13] = 0x02; // Options
        report[16] = report[17] = 255; // both triggers
        pass = WriteFile(writer, report, sizeof(report), &written, nullptr) && pass;
        pass = (source.Poll(state) == InputPollResult::Sample
            && readButtons() == (kBtnCross | kBtnTouchpad | kBtnOptions | kBtnL2 | kBtnR2)) && pass;
        report[12] = 0x10;
        report[13] = report[16] = report[17] = 0;
        pass = WriteFile(writer, report, sizeof(report), &written, nullptr) && pass;
        pass = (source.Poll(state) == InputPollResult::Sample && readButtons() == kBtnCross) && pass;
        report[3] = 0x1C; // renewal reply must not create a release or press
        pass = WriteFile(writer, report, sizeof(report), &written, nullptr) && pass;
        pass = (source.Poll(state) == InputPollResult::NoSample && readButtons() == kBtnCross) && pass;
        report[3] = 0xEF;
        report[12] = 0;
        pass = WriteFile(writer, report, sizeof(report), &written, nullptr) && pass;
        pass = (source.Poll(state) == InputPollResult::Sample && readButtons() == 0) && pass;
        // Destruction must cancel/complete a still-pending read without a peer write.
        pass = (source.Poll(state) == InputPollResult::NoSample) && pass;
    }
    CloseHandle(writer);
    CloseHandle(pipe);
    std::printf("MI01_READ_SELFTEST result=%s held_frames=5000 tap_pairs=1000 checks=chords,renewal_filter,pending_cleanup\n", pass ? "PASS" : "FAIL");
    return pass ? 0 : 1;
}

class XInputOrdinaryInputSource final : public OrdinaryInputSource
{
public:
    const char* name() const override
    {
        return "xinput";
    }

    int deviceIndex() const override
    {
        return slot_ < XUSER_MAX_COUNT ? static_cast<int>(slot_) : -1;
    }

    InputPollResult Poll(NormalizedInputState& state) override
    {
        if (slot_ >= XUSER_MAX_COUNT) {
            for (DWORD candidate = 0; candidate < XUSER_MAX_COUNT; ++candidate) {
                XINPUT_STATE candidateState = {};
                if (XInputGetState(candidate, &candidateState) == ERROR_SUCCESS) {
                    slot_ = candidate;
                    state = NormalizeXInputState(candidateState.Gamepad);
                    return InputPollResult::Sample;
                }
            }
            return InputPollResult::Disconnected;
        }

        XINPUT_STATE xinput = {};
        if (XInputGetState(slot_, &xinput) != ERROR_SUCCESS) {
            slot_ = XUSER_MAX_COUNT;
            return InputPollResult::Disconnected;
        }
        state = NormalizeXInputState(xinput.Gamepad);
        return InputPollResult::Sample;
    }

private:
    DWORD slot_ = XUSER_MAX_COUNT;
};

class LiveHapticTraceWriter
{
public:
    bool Start(const std::string& path, std::string& error)
    {
        if (path.empty()) {
            return true;
        }
        path_ = path;
        file_.open(path_, std::ios::binary);
        if (!file_) {
            error = "cannot open live trace: " + path_;
            return false;
        }
        if (!vaderhaptics::WriteTraceHeader(file_)) {
            error = "cannot write live trace header: " + path_;
            file_.close();
            return false;
        }
        slots_.resize(kCapacity);
        enabled_ = true;
        writer_ = std::thread([this]() { WriterLoop(); });
        return true;
    }

    bool TryPushFrame(const vaderhaptics::VpcmFrame& frame,
        int64_t timestampUs)
    {
        if (!enabled_ || frame.type != 0x84 || frame.payload.size() != 474) {
            return true;
        }
        return TryPush(timestampUs, frame.generation, frame.type,
            frame.sequence, frame.payload.data(), frame.payload.size());
    }

    bool TryPushInput(int64_t timestampUs, uint8_t leftTrigger,
        uint8_t rightTrigger)
    {
        const uint8_t payload[2] = {leftTrigger, rightTrigger};
        return TryPush(timestampUs, 0, 0xF0, 0, payload, sizeof(payload));
    }

    void Stop()
    {
        if (!enabled_) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stop_ = true;
        }
        condition_.notify_all();
        if (writer_.joinable()) {
            writer_.join();
        }
        vaderhaptics::CaptureTraceRecord health;
        health.timestampUs = maxTimestampUs_;
        health.kind = vaderhaptics::CaptureRecordKind::Health;
        const std::string healthText =
            "live_trace_records=" + std::to_string(records_.load()) +
            " dropped=" + std::to_string(dropped_.load()) +
            " writer_failed=" + (failed_.load() ? "1" : "0") +
            " raw_0x84=1 input_pseudo_type=0xF0 input_rate_hz=50\n";
        health.payload.assign(healthText.begin(), healthText.end());
        if (!vaderhaptics::AppendTraceRecord(file_, health)) {
            failed_.store(true);
        }
        else {
            ++records_;
        }
        file_.flush();
        file_.close();
        enabled_ = false;
    }

    ~LiveHapticTraceWriter()
    {
        Stop();
    }

    bool enabled() const { return enabled_; }
    bool failed() const { return failed_.load(); }
    uint64_t records() const { return records_.load(); }
    uint64_t dropped() const { return dropped_.load(); }
    const std::string& path() const { return path_; }

private:
    static constexpr size_t kCapacity = 8192;
    static constexpr size_t kMaxPayload = 474;

    struct Slot
    {
        int64_t timestampUs = 0;
        uint32_t generation = 0;
        uint8_t type = 0;
        uint32_t sequence = 0;
        uint16_t size = 0;
        std::array<uint8_t, kMaxPayload> payload{};
    };

    bool TryPush(int64_t timestampUs, uint32_t generation, uint8_t type,
        uint32_t sequence, const uint8_t* payload, size_t size)
    {
        if (!enabled_ || payload == nullptr || size > kMaxPayload) {
            return false;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (count_ == slots_.size()) {
                ++dropped_;
                return false;
            }
            Slot& slot = slots_[tail_];
            slot.timestampUs = timestampUs;
            slot.generation = generation;
            slot.type = type;
            slot.sequence = sequence;
            slot.size = static_cast<uint16_t>(size);
            std::memcpy(slot.payload.data(), payload, size);
            maxTimestampUs_ = std::max(maxTimestampUs_, timestampUs);
            tail_ = (tail_ + 1) % slots_.size();
            ++count_;
        }
        condition_.notify_one();
        return true;
    }

    void WriterLoop()
    {
        for (;;) {
            Slot slot;
            {
                std::unique_lock<std::mutex> lock(mutex_);
                condition_.wait(lock, [&]() { return stop_ || count_ > 0; });
                if (count_ == 0 && stop_) {
                    break;
                }
                slot = slots_[head_];
                head_ = (head_ + 1) % slots_.size();
                --count_;
            }
            vaderhaptics::CaptureTraceRecord record;
            record.timestampUs = slot.timestampUs;
            record.generation = slot.generation;
            record.kind = vaderhaptics::CaptureRecordKind::Frame;
            record.type = slot.type;
            record.sequence = slot.sequence;
            record.payload.assign(
                slot.payload.begin(), slot.payload.begin() + slot.size);
            if (!vaderhaptics::AppendTraceRecord(file_, record)) {
                failed_.store(true);
            }
            else {
                ++records_;
            }
        }
    }

    std::string path_;
    std::ofstream file_;
    std::vector<Slot> slots_;
    std::mutex mutex_;
    std::condition_variable condition_;
    size_t head_ = 0;
    size_t tail_ = 0;
    size_t count_ = 0;
    int64_t maxTimestampUs_ = 0;
    bool stop_ = false;
    bool enabled_ = false;
    std::thread writer_;
    std::atomic<bool> failed_{false};
    std::atomic<uint64_t> records_{0};
    std::atomic<uint64_t> dropped_{0};
};

bool ReadVirtualDs5Report(uint8_t out[64], DWORD timeoutMs = 2000);

int RunBridge(int durationSeconds, const std::string& configPath,
    bool motorOutputEnabled, const std::string& axisCsvPath,
    InputSourceKind inputSourceKind, const std::string& tracePath,
    const std::string& flydigiOnboardDirectory)
{
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    HidDeviceInfo vader;
    HANDLE vaderHandle = INVALID_HANDLE_VALUE;
    VaderState initialVaderState;
    if (!OpenLiveVaderMi01(vader, vaderHandle, initialVaderState)) {
        std::printf("ERROR: no live Vader 5 Pro MI_01 candidate passed "
                    "GET_INFO/status/ACQUIRE/0xEF validation.\n");
        return 1;
    }
    VaderOwnershipGuard ownershipGuard;
    vaderhaptics::input::FlydigiOnboardProfile expansionProfile;
    const bool expansionProfileLoaded = inputSourceKind == InputSourceKind::Mi01 &&
        LoadFlydigiExpansionProfile(vaderHandle,
            vader.outputReportByteLength, flydigiOnboardDirectory,
            expansionProfile);

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("ERROR: WSAStartup failed\n");
        CloseHandle(vaderHandle);
        return 1;
    }

    BridgeState state;
    state.bridgePid = static_cast<uint32_t>(GetCurrentProcessId());
    if (!EnsureViiperServer(state)) {
        WSACleanup();
        CloseHandle(vaderHandle);
        return 1;
    }
    SOCKET stream = INVALID_SOCKET;
    if (!CreatePersona(state, stream)) {
        DestroyPersona(state, stream);
        WSACleanup();
        CloseHandle(vaderHandle);
        return 1;
    }
    uint8_t neutralReadback[64] = {};
    if (!ReadVirtualDs5Report(neutralReadback, 3000)) {
        std::printf("ERROR: virtual DualSense neutral readback was not available; "
                    "physical visibility remains untouched\n");
        DestroyPersona(state, stream);
        WSACleanup();
        CloseHandle(vaderHandle);
        return 1;
    }
    std::printf("virtual DualSense neutral readback: PASS\n");

    if (inputSourceKind == InputSourceKind::Mi01) {
        if (!EnterPhysicalGamePersonaIsolation(state)) {
            DestroyPersona(state, stream);
            WSACleanup();
            CloseHandle(vaderHandle);
            return 1;
        }
        CloseHandle(vaderHandle);
        vaderHandle = INVALID_HANDLE_VALUE;
        Sleep(200);
        if (!OpenLiveVaderMi01(vader, vaderHandle, initialVaderState)) {
            std::printf("ERROR: MI_01 could not be reopened after visibility "
                        "isolation; "
                        "rolling back the session\n");
            DestroyPersona(state, stream);
            WSACleanup();
            return 1;
        }
        std::printf("post-isolation MI_01 reopen/ACQUIRE/0xEF: PASS\n");
    }
    else {
        std::printf("WARNING: explicit XInput diagnostic source uses the legacy "
                    "external visibility path; integrated dynamic HidHide is "
                    "MI_01-only\n");
    }

    std::unique_ptr<OrdinaryInputSource> inputSourceStorage;
    if (inputSourceKind == InputSourceKind::XInput) {
        inputSourceStorage = std::make_unique<XInputOrdinaryInputSource>();
    }
    else {
        inputSourceStorage = std::make_unique<Mi01OrdinaryInputSource>(
            vaderHandle, initialVaderState,
            expansionProfileLoaded ? &expansionProfile : nullptr);
    }
    OrdinaryInputSource* inputSource = inputSourceStorage.get();
    NormalizedInputState initialInputState;
    if (inputSource->Poll(initialInputState) != InputPollResult::Sample) {
        std::printf("ERROR: ordinary input source '%s' unavailable\n",
            inputSource->name());
        DestroyPersona(state, stream);
        WSACleanup();
        CloseHandle(vaderHandle);
        return 1;
    }
    std::printf("ordinary input source: %s deviceIndex=%d "
                "deadzone=none curve=none\n",
        inputSource->name(), inputSource->deviceIndex());

    HANDLE outputHandle = OpenSeparateVaderOutputHandle(vader);
    const bool separateOutputHandle = outputHandle != INVALID_HANDLE_VALUE;
    if (!separateOutputHandle) {
        outputHandle = vaderHandle;
    }
    if (g_nodelay) {
        const BOOL enabled = TRUE;
        setsockopt(stream, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&enabled), sizeof(enabled));
        std::printf("TCP_NODELAY=1 enabled\n");
    }

    std::ofstream axisCsv;
    if (!axisCsvPath.empty()) {
        axisCsv.open(axisCsvPath, std::ios::out | std::ios::trunc);
        if (!axisCsv) {
            std::printf("ERROR: cannot open axis CSV: %s\n", axisCsvPath.c_str());
            DestroyPersona(state, stream);
            WSACleanup();
            if (separateOutputHandle) CloseHandle(outputHandle);
            CloseHandle(vaderHandle);
            return 1;
        }
        axisCsv << "t_us,source_lx,source_ly,source_rx,source_ry,"
                   "mapped_lx,mapped_ly,mapped_rx,mapped_ry,"
                   "usb_lx,usb_ly,usb_rx,usb_ry,usb_valid\n";
        std::printf("axis diagnostic CSV: %s (100 ms sampling)\n",
            axisCsvPath.c_str());
    }

    uint32_t sequence = 0;
    uint64_t sentFrames = 0;
    uint64_t inputSamples = 1;
    uint64_t mi01Reports = inputSourceKind == InputSourceKind::Mi01 ? 1 : 0;
    uint64_t rightSamples = initialInputState.lx >= 20000 ? 1 : 0;
    uint64_t feedbackBytes = 0;
    uint64_t feedbackFrames = 0;
    uint8_t lastPayload[33] = {};
    MapToDs5(initialInputState, lastPayload);
    uint32_t lastEdgeButtons = static_cast<uint32_t>(lastPayload[4]) |
        (static_cast<uint32_t>(lastPayload[5]) << 8) |
        (static_cast<uint32_t>(lastPayload[6]) << 16) |
        (static_cast<uint32_t>(lastPayload[7]) << 24);
    InputCoverage inputCoverage;
    UpdateInputCoverage(inputCoverage, initialInputState,
        lastEdgeButtons, false);
    uint8_t lastEdgeDpad = initialInputState.dpad;
    uint8_t lastEdgeVendorButtons = initialInputState.vendorButtons;
    bool lastEdgeLyActive =
        std::abs(static_cast<int>(initialInputState.ly)) >= 16384;
    NormalizedInputState lastInputState = initialInputState;
    int16_t lastSourceLx = initialInputState.lx;
    bool sentInitial = false;
    bool runtimeFailed = false;
    bool inputFailsafeActive = false;
    uint64_t inputNeutralizations = 0;
    int64_t maxInputGapUs = 0;

    auto initialFrame = BuildVpcmInputFrame(sequence++, lastPayload);
    int initialSent = send(stream,
        reinterpret_cast<const char*>(initialFrame.data()),
        static_cast<int>(initialFrame.size()), 0);
    if (initialSent == static_cast<int>(initialFrame.size())) {
        ++sentFrames;
        sentInitial = true;
    }
    std::printf("initial live input frame sent: %s (source=%s LX=%d)\n",
        sentInitial ? "yes" : "no", inputSource->name(), lastSourceLx);
    PrintVirtualDualSenseBounded();

    auto start = std::chrono::steady_clock::now();
    auto lastDiag = start;
    auto lastSend = start;
    auto lastInputSample = start;
    auto nextAxisSample = start;
    auto nextTraceInputSample = start;

    LiveHapticTraceWriter traceWriter;
    std::string traceError;
    if (!tracePath.empty()) {
        if (!traceWriter.Start(tracePath, traceError)) {
            std::printf("ERROR: %s\n", traceError.c_str());
            runtimeFailed = true;
        }
        else {
            traceWriter.TryPushInput(
                0, initialInputState.l2, initialInputState.r2);
            std::printf("live haptic trace: %s (raw 0x84 + 50 Hz "
                        "L2/R2 samples)\n",
                traceWriter.path().c_str());
        }
    }

    // Production renderer: spatial stereo grip envelopes plus semantic
    // trigger-vibration substitution -> one bounded four-channel output.
    vaderhaptics::m3::RendererConfig rendererConfig;
    std::string configError;
    if (!vaderhaptics::m3::LoadCalibrationConfig(
            ResolveM3ConfigPath(configPath), rendererConfig, configError)) {
        std::printf("WARNING: M3 calibration fallback: %s\n",
            configError.c_str());
    }
    const char* policyName = rendererConfig.policy ==
            vaderhaptics::m3::RendererPolicy::SpectralMono
        ? "spectral-mono"
        : "stereo-envelope";
    std::printf("M3 renderer: policy=%s sourceGain=%.3f minDrive=%.4f "
                "limiterCap=%.4f gamma=%.3f\n",
        policyName, rendererConfig.sourceGain, rendererConfig.minDrive,
        rendererConfig.limiterCap, rendererConfig.gamma);
    vaderhaptics::m3::HapticRenderer renderer(rendererConfig);
    vaderhaptics::m3::TriggerSemanticAdapter triggerAdapter(rendererConfig);
    vaderhaptics::m3::RumbleOutputBackend outputBackend(!motorOutputEnabled, 0);
    outputBackend.SetDeviceHandle(outputHandle);
    outputBackend.Start();
    std::printf("M3 motor output: %s\n", motorOutputEnabled ? "on" : "off");
    vaderhaptics::m3::VaderHapticFrame initialZero;
    initialZero.timestampUs = 0;
    outputBackend.Enqueue(initialZero);

    M3LiveTelemetry telemetry;
    telemetry.processingNs.reserve(4096);
    vaderhaptics::VpcmParser::FrameCallback onFrame =
        [&](const vaderhaptics::VpcmFrame& frame) {
            if (frame.type != 0x84) {
                return;
            }
            const int64_t frameTimestampUs = std::chrono::duration_cast<
                std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - start).count();
            traceWriter.TryPushFrame(frame, frameTimestampUs);
            ++telemetry.blocksReceived84;
            vaderhaptics::m3::DualSenseHapticsBlock block;
            if (!vaderhaptics::m3::DecodeDualSenseHapticsBlock(
                    frame.payload.data(), frame.payload.size(), block)) {
                ++telemetry.decodeFailures;
                return;
            }
            ++telemetry.blocksDecoded;
            block.generation = frame.generation;
            block.sequence = frame.sequence;
            block.timestampUs = frameTimestampUs;
            bool allZero = true;
            for (const auto& sample : block.samples) {
                if (sample.left != 0 || sample.right != 0) {
                    allZero = false;
                    break;
                }
            }
            if (allZero) {
                ++telemetry.zeroBlocks;
            }
            else {
                ++telemetry.activeBlocks;
            }
            const auto processStart = std::chrono::steady_clock::now();
            vaderhaptics::m3::VaderHapticFrame drive = renderer.Process(block);
            triggerAdapter.Process(
                block, lastInputState.l2, lastInputState.r2, drive);
            vaderhaptics::m3::LimitVaderHapticFrame(
                drive, rendererConfig.limiterCap);
            if (inputFailsafeActive) {
                drive = vaderhaptics::m3::VaderHapticFrame{};
                drive.timestampUs = block.timestampUs;
            }
            const auto processEnd = std::chrono::steady_clock::now();
            const auto processingNs = static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(
                    processEnd - processStart)
                    .count());
            if (telemetry.processingNs.size() < 4096) telemetry.processingNs.push_back(processingNs);
            else telemetry.processingNs[telemetry.rendererProcessed % 4096] = processingNs;
            ++telemetry.rendererProcessed;
            telemetry.last0x84Us = std::chrono::duration_cast<
                std::chrono::microseconds>(processEnd - start).count();
            telemetry.leftGripMax = std::max(telemetry.leftGripMax,
                static_cast<double>(drive.leftGrip));
            telemetry.rightGripMax = std::max(telemetry.rightGripMax,
                static_cast<double>(drive.rightGrip));
            telemetry.leftTriggerMax = std::max(telemetry.leftTriggerMax,
                static_cast<double>(drive.leftTrigger));
            telemetry.rightTriggerMax = std::max(telemetry.rightTriggerMax,
                static_cast<double>(drive.rightTrigger));
            telemetry.leftGripMeanSum += drive.leftGrip;
            telemetry.rightGripMeanSum += drive.rightGrip;
            const double peak = std::max({
                static_cast<double>(drive.leftGrip),
                static_cast<double>(drive.rightGrip),
                static_cast<double>(drive.leftTrigger),
                static_cast<double>(drive.rightTrigger)});
            if (peak > 0.0) {
                ++telemetry.nonzeroBlocks;
            }
            if (peak >= rendererConfig.limiterCap - 1e-6) {
                ++telemetry.capBlocks;
            }
            outputBackend.Enqueue(drive);
        };
    vaderhaptics::VpcmParser::ErrorCallback onError =
        [](vaderhaptics::VpcmError) {};
    vaderhaptics::VpcmParser parser(onFrame, onError);

    VaderAcquireHeartbeat acquireHeartbeat;
    if (!acquireHeartbeat.Start(vader)) {
        std::printf("ERROR: could not start Vader acquisition renewal worker\n");
        runtimeFailed = true;
        telemetry.runtimeFailed = true;
    }
    while (!g_interrupted) {
        if (acquireHeartbeat.failed()) {
            std::printf("ERROR: Vader acquisition renewal failed; stopping safely\n");
            runtimeFailed = true;
            telemetry.runtimeFailed = true;
        }
        if (runtimeFailed || traceWriter.failed()) {
            if (traceWriter.failed()) {
                std::printf("ERROR: live haptic trace writer failed\n");
                runtimeFailed = true;
                telemetry.runtimeFailed = true;
            }
            break;
        }
        if (g_bridgeStopEvent &&
            WaitForSingleObject(g_bridgeStopEvent, 0) == WAIT_OBJECT_0) {
            std::printf("graceful stop requested\n");
            break;
        }
        auto now = std::chrono::steady_clock::now();
        double elapsed =
            std::chrono::duration<double>(now - start).count();
        if (durationSeconds > 0 && elapsed >= durationSeconds) {
            break;
        }

        NormalizedInputState inputState;
        const InputPollResult pollResult = inputSource->Poll(inputState);
        now = std::chrono::steady_clock::now();
        const bool decoded = pollResult == InputPollResult::Sample;
        bool forceNeutralFrame = false;
        if (pollResult == InputPollResult::Disconnected) {
            std::printf("ERROR: ordinary input source '%s' disconnected; "
                        "stopping instead of forwarding stale input\n",
                inputSource->name());
            runtimeFailed = true;
            telemetry.runtimeFailed = true;
            break;
        }
        if (decoded) {
            if (inputFailsafeActive) {
                const int64_t recoveredUs = std::chrono::duration_cast<
                    std::chrono::microseconds>(now - lastInputSample).count();
                maxInputGapUs = std::max(maxInputGapUs, recoveredUs);
                if (g_inputDiagnostics) std::printf(
                    "ordinary input source '%s' recovered after %lld us; "
                    "resuming live input and haptics\n",
                    inputSource->name(), static_cast<long long>(recoveredUs));
                inputFailsafeActive = false;
            }
            lastInputSample = now;
        }
        else {
            const int64_t staleUs = std::chrono::duration_cast<
                std::chrono::microseconds>(now - lastInputSample).count();
            maxInputGapUs = std::max(maxInputGapUs, staleUs);
            if (InputStreamMustStop(staleUs)) {
                std::printf(
                    "ERROR: ordinary input source '%s' produced no sample "
                    "for %lld us; sustained outage, stopping safely\n",
                    inputSource->name(), static_cast<long long>(staleUs));
                outputBackend.StopAllMotors();
                runtimeFailed = true;
                telemetry.runtimeFailed = true;
                break;
            }
            if (InputStreamNeedsNeutral(staleUs) && !inputFailsafeActive) {
                if (g_inputDiagnostics) std::printf(
                    "WARNING: ordinary input source '%s' produced no sample "
                    "for %lld us; sending neutral input and zero haptics while "
                    "waiting for recovery\n",
                    inputSource->name(), static_cast<long long>(staleUs));
                inputFailsafeActive = true;
                ++inputNeutralizations;
                outputBackend.StopAllMotors();
                lastInputState = NormalizedInputState{};
                lastSourceLx = 0;
                MapToDs5(lastInputState, lastPayload);
                forceNeutralFrame = true;
            }
        }
        if (outputBackend.writeFailed()) {
            std::printf("ERROR: physical haptic write failed (Windows error "
                        "%u); forcing zero and stopping the bridge\n",
                outputBackend.lastWriteError());
            outputBackend.StopAllMotors();
            runtimeFailed = true;
            telemetry.runtimeFailed = true;
            break;
        }
        uint8_t payload[33] = {};
        bool stateChanged = false;
        if (decoded) {
            ++inputSamples;
            if (inputSourceKind == InputSourceKind::Mi01) {
                ++mi01Reports;
            }
            lastInputState = inputState;
            if (inputState.lx >= 20000) {
                ++rightSamples;
            }
            lastSourceLx = inputState.lx;
            MapToDs5(inputState, payload);
            stateChanged = std::memcmp(payload, lastPayload, 33) != 0;
            std::memcpy(lastPayload, payload, 33);
            if (traceWriter.enabled() && now >= nextTraceInputSample) {
                const int64_t inputTimestampUs = std::chrono::duration_cast<
                    std::chrono::microseconds>(now - start).count();
                traceWriter.TryPushInput(
                    inputTimestampUs, inputState.l2, inputState.r2);
                nextTraceInputSample =
                    now + std::chrono::milliseconds(20);
            }
            const uint32_t edgeButtons = static_cast<uint32_t>(payload[4]) |
                (static_cast<uint32_t>(payload[5]) << 8) |
                (static_cast<uint32_t>(payload[6]) << 16) |
                (static_cast<uint32_t>(payload[7]) << 24);
            UpdateInputCoverage(inputCoverage, inputState,
                edgeButtons, stateChanged);
            const bool buttonsChanged = edgeButtons != lastEdgeButtons;
            const bool dpadChanged = inputState.dpad != lastEdgeDpad;
            const bool vendorButtonsChanged =
                inputState.vendorButtons != lastEdgeVendorButtons;
            const bool lyActive =
                std::abs(static_cast<int>(inputState.ly)) >= 16384;
            const bool lyChanged = lyActive != lastEdgeLyActive;
            if (g_inputDiagnostics && (buttonsChanged || dpadChanged || vendorButtonsChanged ||
                lyChanged)) {
                std::printf(
                    "EDGE t=%6.3f source=%s buttons=0x%04X dpad=0x%02X "
                    "vendor=0x%02X ly=%d\n",
                    elapsed, inputSource->name(), edgeButtons,
                    inputState.dpad, inputState.vendorButtons, inputState.ly);
            }
            lastEdgeButtons = edgeButtons;
            lastEdgeDpad = inputState.dpad;
            lastEdgeVendorButtons = inputState.vendorButtons;
            lastEdgeLyActive = lyActive;
        }

        bool shouldSend = forceNeutralFrame ||
            (decoded && (stateChanged || !sentInitial));
        if (!shouldSend && decoded &&
            now - lastSend >= std::chrono::milliseconds(100)) {
            shouldSend = true; // periodic heartbeat keeps state fresh
        }
        if (shouldSend) {
            auto frame = BuildVpcmInputFrame(sequence++, lastPayload);
            int sent = send(stream,
                reinterpret_cast<const char*>(frame.data()),
                static_cast<int>(frame.size()), 0);
            if (sent == static_cast<int>(frame.size())) {
                ++sentFrames;
                sentInitial = true;
                lastSend = now;
            }
            else {
                std::printf("ERROR: stream send failed (%d)\n",
                    WSAGetLastError());
                runtimeFailed = true;
                telemetry.runtimeFailed = true;
                break;
            }
        }

        if (axisCsv && now >= nextAxisSample) {
            uint8_t virtualReport[64] = {};
            const bool virtualValid = ReadVirtualDs5Report(virtualReport, 25);
            const auto sampleTime = std::chrono::steady_clock::now();
            const int64_t timestampUs = std::chrono::duration_cast<
                std::chrono::microseconds>(sampleTime - start).count();
            axisCsv << timestampUs << ','
                    << lastInputState.lx << ',' << lastInputState.ly << ','
                    << lastInputState.rx << ',' << lastInputState.ry << ','
                    << static_cast<int>(static_cast<int8_t>(lastPayload[0])) << ','
                    << static_cast<int>(static_cast<int8_t>(lastPayload[1])) << ','
                    << static_cast<int>(static_cast<int8_t>(lastPayload[2])) << ','
                    << static_cast<int>(static_cast<int8_t>(lastPayload[3])) << ','
                    << static_cast<unsigned int>(virtualReport[1]) << ','
                    << static_cast<unsigned int>(virtualReport[2]) << ','
                    << static_cast<unsigned int>(virtualReport[3]) << ','
                    << static_cast<unsigned int>(virtualReport[4]) << ','
                    << (virtualValid ? 1 : 0) << '\n';
            axisCsv.flush();
            nextAxisSample = sampleTime + std::chrono::milliseconds(100);
        }

        // Drain VIIPER feedback through the persistent VPCM parser (M3.6
        // renderer clock). Non-blocking; the parser callback feeds the
        // renderer and the bounded output backend.
        u_long available = 0;
        if (ioctlsocket(stream, FIONREAD, &available) == 0 && available > 0) {
            std::array<char, 16384> buffer;
            int n = recv(stream, buffer.data(),
                static_cast<int>((std::min)(static_cast<u_long>(buffer.size()), available)), 0);
            if (n > 0) {
                feedbackBytes += static_cast<uint64_t>(n);
                parser.Feed(
                    reinterpret_cast<const uint8_t*>(buffer.data()),
                    static_cast<size_t>(n));
            }
            else if (n == 0 ||
                (WSAGetLastError() != WSAETIMEDOUT &&
                    WSAGetLastError() != WSAEWOULDBLOCK)) {
                std::printf("ERROR: VIIPER feedback stream failed\n");
                parser.ResetGeneration();
                renderer.Reset();
                triggerAdapter.Reset();
                outputBackend.StopAllMotors();
                ++telemetry.generationResets;
                telemetry.runtimeFailed = true;
                break;
            }
        }

        // Watchdog: stale 0x84 source must not hold a nonzero drive.
        const int64_t nowUs = std::chrono::duration_cast<
            std::chrono::microseconds>(now - start).count();
        if (telemetry.last0x84Us != 0 &&
            nowUs - telemetry.last0x84Us > 120000) {
            vaderhaptics::m3::VaderHapticFrame zeroDrive;
            zeroDrive.timestampUs = nowUs;
            outputBackend.Enqueue(zeroDrive);
            ++telemetry.watchdogZeroEvents;
            telemetry.last0x84Us = nowUs;
        }

        if (g_inputDiagnostics && now - lastDiag >= std::chrono::seconds(1)) {
            lastDiag = now;
            std::printf(
                "[t=%6.1fs] inputSource=%s inputSamples=%llu sentFrames=%llu "
                "feedbackBytes=%llu haptics84=%llu buttons=0x%02X%02X "
                "lx=%4d ly=%4d rx=%4d ry=%4d l2=%3u r2=%3u\n",
                elapsed,
                inputSource->name(),
                static_cast<unsigned long long>(inputSamples),
                static_cast<unsigned long long>(sentFrames),
                static_cast<unsigned long long>(feedbackBytes),
                static_cast<unsigned long long>(telemetry.blocksDecoded),
                lastPayload[5], lastPayload[4],
                static_cast<int8_t>(lastPayload[0]),
                static_cast<int8_t>(lastPayload[1]),
                static_cast<int8_t>(lastPayload[2]),
                static_cast<int8_t>(lastPayload[3]),
                lastPayload[9], lastPayload[10]);
        }
        if (inputSourceKind == InputSourceKind::XInput) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }

    // Zero output immediately, then stop renewal before releasing ownership.
    outputBackend.StopAllMotors();
    outputBackend.Stop();
    acquireHeartbeat.Stop();
    std::printf("VADER_ACQUIRE_HEARTBEAT renewals=%u failed=%u\n",
        acquireHeartbeat.renewals(), acquireHeartbeat.failed() ? 1u : 0u);
    // M3.6 safety shutdown: reset renderer after output has stopped.
    renderer.Reset();
    triggerAdapter.Reset();
    traceWriter.Stop();
    if (!tracePath.empty()) {
        std::printf(
            "LIVE_TRACE_SUMMARY path=%s records=%llu dropped=%llu failed=%u\n",
            traceWriter.path().c_str(),
            static_cast<unsigned long long>(traceWriter.records()),
            static_cast<unsigned long long>(traceWriter.dropped()),
            traceWriter.failed() ? 1u : 0u);
    }

    const auto& outStats = outputBackend.stats();
    telemetry.enqueued = outStats.enqueued;
    telemetry.written = outStats.written;
    telemetry.droppedStale = outStats.droppedStale;
    telemetry.coalesced = outStats.coalesced;
    telemetry.writeFailures = outStats.writeFailures;
    telemetry.suppressedRedundantZero = outStats.suppressedRedundantZero;
    telemetry.suppressedRedundantState = outStats.suppressedRedundantState;
    telemetry.maxQueueDepth = outStats.maxQueueDepth;
    const double leftGripMean = telemetry.rendererProcessed > 0
        ? telemetry.leftGripMeanSum /
            static_cast<double>(telemetry.rendererProcessed)
        : 0.0;
    const double rightGripMean = telemetry.rendererProcessed > 0
        ? telemetry.rightGripMeanSum /
            static_cast<double>(telemetry.rendererProcessed)
        : 0.0;
    const double nonzeroFraction = telemetry.rendererProcessed > 0
        ? static_cast<double>(telemetry.nonzeroBlocks) /
            static_cast<double>(telemetry.rendererProcessed)
        : 0.0;
    const double capFraction = telemetry.rendererProcessed > 0
        ? static_cast<double>(telemetry.capBlocks) /
            static_cast<double>(telemetry.rendererProcessed)
        : 0.0;
    std::printf(
        "M3_LIVE_SUMMARY policy=%s blocks_received_0x84=%llu "
        "blocks_decoded=%llu decode_failures=%llu active=%llu zero=%llu "
        "renderer_processed=%llu generation_resets=%llu enqueued=%llu "
        "physical_writes=%llu dropped_stale=%llu coalesced=%llu "
        "suppressed_redundant_zero=%llu suppressed_redundant_state=%llu "
        "max_queue_depth=%llu "
        "watchdog_zero_events=%llu write_failures=%llu last_write_error=%u "
        "processing_p50_ns=%.0f processing_p95_ns=%.0f processing_p99_ns=%.0f "
        "enqueue_to_write_p50_us=%.0f enqueue_to_write_p95_us=%.0f "
        "enqueue_to_write_p99_us=%.0f left_grip_max=%.4f left_grip_mean=%.4f "
        "right_grip_max=%.4f right_grip_mean=%.4f "
        "left_trigger_max=%.4f right_trigger_max=%.4f "
        "nonzero_output_fraction=%.4f "
        "cap_output_fraction=%.4f limiter_cap=%.4f runtime_failed=%u\n",
        policyName,
        static_cast<unsigned long long>(telemetry.blocksReceived84),
        static_cast<unsigned long long>(telemetry.blocksDecoded),
        static_cast<unsigned long long>(telemetry.decodeFailures),
        static_cast<unsigned long long>(telemetry.activeBlocks),
        static_cast<unsigned long long>(telemetry.zeroBlocks),
        static_cast<unsigned long long>(telemetry.rendererProcessed),
        static_cast<unsigned long long>(telemetry.generationResets),
        static_cast<unsigned long long>(telemetry.enqueued),
        static_cast<unsigned long long>(telemetry.written),
        static_cast<unsigned long long>(telemetry.droppedStale),
        static_cast<unsigned long long>(telemetry.coalesced),
        static_cast<unsigned long long>(telemetry.suppressedRedundantZero),
        static_cast<unsigned long long>(telemetry.suppressedRedundantState),
        static_cast<unsigned long long>(telemetry.maxQueueDepth),
        static_cast<unsigned long long>(telemetry.watchdogZeroEvents),
        static_cast<unsigned long long>(telemetry.writeFailures),
        outputBackend.lastWriteError(),
        Percentile(telemetry.processingNs, 0.50),
        Percentile(telemetry.processingNs, 0.95),
        Percentile(telemetry.processingNs, 0.99),
        Percentile(outStats.latencyUs, 0.50),
        Percentile(outStats.latencyUs, 0.95),
        Percentile(outStats.latencyUs, 0.99),
        telemetry.leftGripMax, leftGripMean,
        telemetry.rightGripMax, rightGripMean,
        telemetry.leftTriggerMax, telemetry.rightTriggerMax,
        nonzeroFraction, capFraction, rendererConfig.limiterCap,
        (telemetry.runtimeFailed || runtimeFailed) ? 1u : 0u);

    const double rightPercent = inputSamples > 0
        ? 100.0 * static_cast<double>(rightSamples) /
            static_cast<double>(inputSamples)
        : 0.0;
    std::printf("BRIDGE_SUMMARY inputSource=%s inputSamples=%llu "
                "vaderReports=%llu rightSamples=%llu rightPercent=%.1f "
                "lastSourceLx=%d sentFrames=%llu haptics84=%llu "
                "runtimeFailed=%u changes=%llu buttonsSeen=0x%04X "
                "dpadSeen=0x%02X vendorSeen=0x%02X lxMin=%d lxMax=%d "
                "lyMin=%d lyMax=%d "
                "rxMin=%d rxMax=%d ryMin=%d ryMax=%d l2Max=%u r2Max=%u "
                "inputNeutralizations=%llu maxInputGapUs=%lld\n",
        inputSource->name(),
        static_cast<unsigned long long>(inputSamples),
        static_cast<unsigned long long>(mi01Reports),
        static_cast<unsigned long long>(rightSamples), rightPercent,
        lastSourceLx, static_cast<unsigned long long>(sentFrames),
        static_cast<unsigned long long>(telemetry.blocksDecoded),
        runtimeFailed ? 1u : 0u,
        static_cast<unsigned long long>(inputCoverage.stateChanges),
        inputCoverage.buttonsSeen, inputCoverage.dpadSeen,
        inputCoverage.vendorButtonsSeen,
        inputCoverage.lxMin, inputCoverage.lxMax,
        inputCoverage.lyMin, inputCoverage.lyMax,
        inputCoverage.rxMin, inputCoverage.rxMax,
        inputCoverage.ryMin, inputCoverage.ryMax,
        inputCoverage.l2Max, inputCoverage.r2Max,
        static_cast<unsigned long long>(inputNeutralizations),
        static_cast<long long>(maxInputGapUs));

    inputSourceStorage.reset();
    std::printf("stopping bridge...\n");
    if (separateOutputHandle) {
        CloseHandle(outputHandle);
    }
    CloseHandle(vaderHandle);
    vaderHandle = INVALID_HANDLE_VALUE;
    const bool personaRestored = DestroyPersona(state, stream);
    WSACleanup();

    const bool ownershipReleased = ownershipGuard.ReleaseNow();
    const bool xinputRestored = WaitForFreshProcessXInputCount(1);
    const int virtualCount = CountVirtualDualSenseHidInterfaces();
    const bool visibilityBaseline = VerifyNativeVisibilityBaseline();
    const bool nativePass = personaRestored && ownershipReleased &&
        xinputRestored && virtualCount == 0 && visibilityBaseline;
    std::printf("NATIVE_RESTORE result=%s persona=%u xinput=%u "
                "ownership_released=%u virtual_ds5_interfaces=%d visibility=%u\n",
        nativePass ? "PASS" : "FAIL", personaRestored ? 1u : 0u,
        xinputRestored ? 1u : 0u, ownershipReleased ? 1u : 0u, virtualCount,
        visibilityBaseline ? 1u : 0u);
    std::printf("bridge stopped; native mode restoration %s\n",
        nativePass ? "PASS" : "FAIL");
    PrintVirtualDualSenseBounded();
    return (runtimeFailed || !nativePass) ? 1 : 0;
}

std::string Trim(const std::string& text)
{
    size_t begin = text.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) {
        return {};
    }
    size_t end = text.find_last_not_of(" \t\r\n");
    return text.substr(begin, end - begin + 1);
}

bool ReadVirtualDs5Report(uint8_t out[64], DWORD timeoutMs)
{
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do {
        auto devices = EnumerateHidDevices();
        for (const auto& device : devices) {
            if (device.vid == 0x054C && device.pid == 0x0CE6 &&
                device.usagePage == 0x01 && device.usage == 0x05) {
                HANDLE handle = CreateFileW(device.devicePath.c_str(),
                    GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                    OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
                if (handle == INVALID_HANDLE_VALUE) {
                    continue;
                }
                const ULONGLONG now = GetTickCount64();
                const DWORD remaining = now < deadline
                    ? static_cast<DWORD>(deadline - now)
                    : 0;
                const DWORD readSliceMs = (std::min)(remaining, 250UL);
                DWORD read = 0;
                bool ok = readSliceMs > 0 &&
                    ReadHidWithTimeout(handle, out, 64, readSliceMs, &read) &&
                    read > 0;
                CloseHandle(handle);
                if (ok) {
                    return true;
                }
            }
        }
        if (GetTickCount64() >= deadline) {
            break;
        }
        Sleep(50);
    } while (true);
    return false;
}

void PrintHexInline(const uint8_t* data, size_t size)
{
    for (size_t index = 0; index < size; ++index) {
        std::printf("%02X", data[index]);
    }
}

void LogSyntheticVpcm(const char* label, const uint8_t payload[33])
{
    const uint32_t buttons = static_cast<uint32_t>(payload[4]) |
        (static_cast<uint32_t>(payload[5]) << 8) |
        (static_cast<uint32_t>(payload[6]) << 16) |
        (static_cast<uint32_t>(payload[7]) << 24);
    std::printf(
        "SYNTHETIC_VPCM label=\"%s\" lx=%d ly=%d rx=%d ry=%d "
        "buttons=0x%08X dpad=0x%02X l2=%u r2=%u "
        "touch1=0x%02X touch2=0x%02X gyro=%d,%d,%d accel=%d,%d,%d "
        "payload=",
        label,
        static_cast<int>(static_cast<int8_t>(payload[0])),
        static_cast<int>(static_cast<int8_t>(payload[1])),
        static_cast<int>(static_cast<int8_t>(payload[2])),
        static_cast<int>(static_cast<int8_t>(payload[3])),
        buttons, payload[8], payload[9], payload[10], payload[15],
        payload[20], Load16LE(payload + 21), Load16LE(payload + 23),
        Load16LE(payload + 25), Load16LE(payload + 27),
        Load16LE(payload + 29), Load16LE(payload + 31));
    PrintHexInline(payload, 33);
    std::printf("\n");
}

void LogSyntheticReadback(const char* label)
{
    uint8_t report[64] = {};
    if (!ReadVirtualDs5Report(report)) {
        std::printf("SYNTHETIC_HID label=\"%s\" valid=no\n", label);
        return;
    }
    const uint32_t buttons = static_cast<uint32_t>(report[8] & 0xF0) |
        (static_cast<uint32_t>(report[9]) << 8) |
        (static_cast<uint32_t>(report[10]) << 16);
    const bool touch1Inactive = (report[33] & 0x80) != 0;
    const bool touch2Inactive = (report[37] & 0x80) != 0;
    const int16_t gyroX = Load16LE(report + 16);
    const int16_t gyroY = Load16LE(report + 18);
    const int16_t gyroZ = Load16LE(report + 20);
    const int16_t accelX = Load16LE(report + 22);
    const int16_t accelY = Load16LE(report + 24);
    const int16_t accelZ = Load16LE(report + 26);
    const bool neutral = report[0] == 0x01 &&
        report[1] == 128 && report[2] == 128 &&
        report[3] == 128 && report[4] == 128 &&
        report[5] == 0 && report[6] == 0 &&
        (report[8] & 0x0F) == 0x08 && buttons == 0 &&
        touch1Inactive && touch2Inactive &&
        gyroX == 0 && gyroY == 0 && gyroZ == 0 &&
        accelX == 0 && accelY == 0 && accelZ == -8192;
    std::printf(
        "SYNTHETIC_HID label=\"%s\" valid=yes neutral=%s "
        "report_id=0x%02X lx=%u ly=%u rx=%u ry=%u l2=%u r2=%u "
        "dpad=0x%02X buttons=0x%06X touch1=0x%02X touch2=0x%02X "
        "gyro=%d,%d,%d accel=%d,%d,%d battery=0x%02X report=",
        label, neutral ? "yes" : "no", report[0], report[1], report[2],
        report[3], report[4], report[5], report[6], report[8] & 0x0F,
        buttons, report[33], report[37], gyroX, gyroY, gyroZ,
        accelX, accelY, accelZ, report[53]);
    PrintHexInline(report, sizeof(report));
    std::printf("\n");
}

int RunSynthetic(int durationSeconds, const std::string& commandsPath,
    bool readback)
{
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("ERROR: WSAStartup failed\n");
        return 1;
    }

    BridgeState state;
    if (!EnsureViiperServer(state)) {
        WSACleanup();
        return 1;
    }
    SOCKET stream = INVALID_SOCKET;
    if (!CreatePersona(state, stream)) {
        DestroyPersona(state, stream);
        WSACleanup();
        return 1;
    }
    if (g_nodelay) {
        const BOOL enabled = TRUE;
        if (setsockopt(stream, IPPROTO_TCP, TCP_NODELAY,
                reinterpret_cast<const char*>(&enabled), sizeof(enabled)) !=
            0) {
            std::printf("ERROR: TCP_NODELAY failed (%d)\n",
                WSAGetLastError());
            DestroyPersona(state, stream);
            WSACleanup();
            return 1;
        }
        std::printf("TCP_NODELAY=1 enabled (synthetic)\n");
    }

    uint8_t payload[33] = {};
    uint32_t buttons = 0;
    uint8_t dpad = 0;
    int8_t lx = 0, ly = 0, rx = 0, ry = 0;
    uint8_t l2 = 0, r2 = 0;

    auto applyPayload = [&]() {
        std::memset(payload, 0, 33);
        payload[0] = static_cast<uint8_t>(lx);
        payload[1] = static_cast<uint8_t>(ly);
        payload[2] = static_cast<uint8_t>(rx);
        payload[3] = static_cast<uint8_t>(ry);
        payload[4] = static_cast<uint8_t>(buttons & 0xFF);
        payload[5] = static_cast<uint8_t>((buttons >> 8) & 0xFF);
        payload[6] = static_cast<uint8_t>((buttons >> 16) & 0xFF);
        payload[7] = static_cast<uint8_t>((buttons >> 24) & 0xFF);
        payload[8] = dpad;
        payload[9] = l2;
        payload[10] = r2;
        payload[15] = kTouchInactive;
        payload[20] = kTouchInactive;
        payload[31] = static_cast<uint8_t>(0xE000 & 0xFF);
        payload[32] = static_cast<uint8_t>((0xE000 >> 8) & 0xFF);
    };
    applyPayload();

    uint32_t sequence = 0;
    uint64_t sentFrames = 0;
    uint64_t feedbackBytes = 0;
    uint64_t feedbackFrames = 0;
    bool sentInitial = false;
    auto start = std::chrono::steady_clock::now();
    auto lastSend = start;
    auto lastDiag = start;
    auto lastReadback = start;
    auto lastCmdCheck = start;

    auto sendCurrent = [&]() -> bool {
        auto frame = BuildVpcmInputFrame(sequence++, payload);
        int sent = send(stream,
            reinterpret_cast<const char*>(frame.data()),
            static_cast<int>(frame.size()), 0);
        if (sent == static_cast<int>(frame.size())) {
            ++sentFrames;
            sentInitial = true;
            lastSend = std::chrono::steady_clock::now();
            return true;
        }
        return false;
    };

    auto neutralFrame = BuildVpcmInputFrame(sequence++, payload);
    int neutralSent = send(stream,
        reinterpret_cast<const char*>(neutralFrame.data()),
        static_cast<int>(neutralFrame.size()), 0);
    if (neutralSent == static_cast<int>(neutralFrame.size())) {
        ++sentFrames;
        sentInitial = true;
    }
    std::printf("synthetic neutral input frame sent: %s\n",
        sentInitial ? "yes" : "no");
    LogSyntheticVpcm("initial neutral", payload);
    PrintVirtualDualSenseBounded();
    if (readback) {
        LogSyntheticReadback("initial neutral");
    }

    size_t processedLines = 0;
    bool quit = false;
    bool stickPending = false;
    auto stickRevertAt = std::chrono::steady_clock::time_point{};
    int8_t stickRevert[4] = {};
    bool pulsePending = false;
    auto pulseRevertAt = std::chrono::steady_clock::time_point{};
    uint32_t pulseButtons = 0;
    uint8_t pulseDpad = 0;

    auto handleCommand = [&](const std::string& line) -> bool {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        if (line == "quit") {
            return true;
        }
        if (line == "cross down") {
            buttons |= kBtnCross;
            applyPayload();
            sendCurrent();
            std::printf("[t=%6.1fs] synthetic cross down buttons=0x%08X\n",
                elapsed, buttons);
            LogSyntheticVpcm("cross down", payload);
            if (readback) {
                LogSyntheticReadback("cross down");
            }
            return false;
        }
        if (line == "cross up") {
            buttons &= ~kBtnCross;
            applyPayload();
            sendCurrent();
            std::printf("[t=%6.1fs] synthetic cross up buttons=0x%08X\n",
                elapsed, buttons);
            LogSyntheticVpcm("cross up", payload);
            if (readback) {
                LogSyntheticReadback("cross up");
            }
            return false;
        }
        const char* dpadNames[] = {"up", "right", "down", "left"};
        const uint8_t dpadBits[] = {0x01, 0x08, 0x02, 0x04};
        for (int i = 0; i < 4; ++i) {
            std::string prefix = std::string("dpad ") + dpadNames[i] + " ";
            if (line.rfind(prefix, 0) == 0) {
                std::string action = line.substr(prefix.size());
                if (action == "down") {
                    dpad |= dpadBits[i];
                }
                else if (action == "up") {
                    dpad &= static_cast<uint8_t>(~dpadBits[i]);
                }
                else {
                    std::printf("[t=%6.1fs] synthetic ignored: %s\n",
                        elapsed, line.c_str());
                    return false;
                }
                applyPayload();
                sendCurrent();
                std::printf("[t=%6.1fs] synthetic %s %s dpad=0x%02X\n",
                    elapsed, prefix.c_str(), action.c_str(), dpad);
                LogSyntheticVpcm(line.c_str(), payload);
                if (readback) {
                    LogSyntheticReadback(line.c_str());
                }
                return false;
            }
        }
        if (line.rfind("stick ", 0) == 0) {
            std::string rest = line.substr(6);
            size_t sp1 = rest.find(' ');
            if (sp1 == std::string::npos) {
                std::printf("[t=%6.1fs] synthetic ignored: %s\n", elapsed,
                    line.c_str());
                return false;
            }
            std::string axis = rest.substr(0, sp1);
            std::string valueText = rest.substr(sp1 + 1);
            size_t sp2 = valueText.find(' ');
            if (sp2 == std::string::npos) {
                std::printf("[t=%6.1fs] synthetic ignored: %s\n", elapsed,
                    line.c_str());
                return false;
            }
            int value = std::atoi(valueText.substr(0, sp2).c_str());
            int holdMs = std::atoi(valueText.substr(sp2 + 1).c_str());
            value = std::clamp(value, -128, 127);
            holdMs = std::clamp(holdMs, 100, 10000);
            stickRevert[0] = lx;
            stickRevert[1] = ly;
            stickRevert[2] = rx;
            stickRevert[3] = ry;
            if (axis == "lx") lx = static_cast<int8_t>(value);
            else if (axis == "ly") ly = static_cast<int8_t>(value);
            else if (axis == "rx") rx = static_cast<int8_t>(value);
            else if (axis == "ry") ry = static_cast<int8_t>(value);
            else {
                std::printf("[t=%6.1fs] synthetic ignored: %s\n", elapsed,
                    line.c_str());
                return false;
            }
            stickPending = true;
            stickRevertAt = now + std::chrono::milliseconds(holdMs);
            applyPayload();
            sendCurrent();
            std::printf("[t=%6.1fs] synthetic stick %s=%d hold=%dms\n",
                elapsed, axis.c_str(), value, holdMs);
            LogSyntheticVpcm(line.c_str(), payload);
            if (readback) {
                LogSyntheticReadback(line.c_str());
            }
            return false;
        }
        if (line.rfind("pulse ", 0) == 0) {
            if (pulsePending) {
                std::printf("[t=%6.1fs] synthetic ignored overlapping pulse: %s\n",
                    elapsed, line.c_str());
                return false;
            }
            const std::string rest = line.substr(6);
            const size_t separator = rest.rfind(' ');
            if (separator == std::string::npos) {
                std::printf("[t=%6.1fs] synthetic ignored: %s\n", elapsed,
                    line.c_str());
                return false;
            }
            const std::string control = rest.substr(0, separator);
            int holdMs = std::atoi(rest.substr(separator + 1).c_str());
            holdMs = std::clamp(holdMs, 50, 10000);
            pulseButtons = buttons;
            pulseDpad = dpad;
            if (control == "cross") {
                buttons |= kBtnCross;
            }
            else if (control == "dpad down") {
                dpad |= 0x02;
            }
            else if (control == "dpad up") {
                dpad |= 0x01;
            }
            else {
                std::printf("[t=%6.1fs] synthetic ignored: %s\n", elapsed,
                    line.c_str());
                return false;
            }
            pulsePending = true;
            pulseRevertAt = now + std::chrono::milliseconds(holdMs);
            applyPayload();
            sendCurrent();
            std::printf("[t=%6.1fs] synthetic pulse %s hold=%dms\n",
                elapsed, control.c_str(), holdMs);
            LogSyntheticVpcm(line.c_str(), payload);
            if (readback) {
                LogSyntheticReadback(line.c_str());
            }
            return false;
        }
        std::printf("[t=%6.1fs] synthetic ignored: %s\n", elapsed,
            line.c_str());
        return false;
    };

    while (!g_interrupted && !quit) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        if (durationSeconds > 0 && elapsed >= durationSeconds) {
            break;
        }

        if (!commandsPath.empty() && now - lastCmdCheck >=
            std::chrono::milliseconds(100)) {
            lastCmdCheck = now;
            std::ifstream file(commandsPath);
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(file, line)) {
                std::string trimmed = Trim(line);
                if (!trimmed.empty() && trimmed[0] != '#') {
                    lines.push_back(trimmed);
                }
            }
            if (lines.size() > processedLines) {
                for (size_t i = processedLines; i < lines.size(); ++i) {
                    std::printf("[t=%6.1fs] synthetic cmd: %s\n",
                        elapsed, lines[i].c_str());
                    if (handleCommand(lines[i])) {
                        quit = true;
                        break;
                    }
                }
                processedLines = lines.size();
            }
        }

        if (stickPending && now >= stickRevertAt) {
            lx = stickRevert[0];
            ly = stickRevert[1];
            rx = stickRevert[2];
            ry = stickRevert[3];
            stickPending = false;
            applyPayload();
            sendCurrent();
            std::printf("[t=%6.1fs] synthetic stick reverted\n", elapsed);
            LogSyntheticVpcm("stick reverted", payload);
            if (readback) {
                LogSyntheticReadback("stick reverted");
            }
        }

        if (pulsePending && now >= pulseRevertAt) {
            buttons = pulseButtons;
            dpad = pulseDpad;
            pulsePending = false;
            applyPayload();
            sendCurrent();
            std::printf("[t=%6.1fs] synthetic pulse reverted\n", elapsed);
            LogSyntheticVpcm("pulse reverted", payload);
            if (readback) {
                LogSyntheticReadback("pulse reverted");
            }
        }

        if (now - lastSend >= std::chrono::milliseconds(100)) {
            if (!sendCurrent()) {
                std::printf("ERROR: stream send failed (%d)\n",
                    WSAGetLastError());
                break;
            }
        }

        u_long available = 0;
        if (ioctlsocket(stream, FIONREAD, &available) == 0 && available > 0) {
            std::vector<char> buffer(available);
            int n = recv(stream, buffer.data(),
                static_cast<int>(buffer.size()), 0);
            if (n > 0) {
                feedbackBytes += static_cast<uint64_t>(n);
                for (int i = 0; i + 3 < n; ++i) {
                    if (buffer[i] == 'V' && buffer[i + 1] == 'P' &&
                        buffer[i + 2] == 'C' && buffer[i + 3] == 'M') {
                        ++feedbackFrames;
                    }
                }
            }
        }

        if (now - lastDiag >= std::chrono::seconds(1)) {
            lastDiag = now;
            std::printf(
                "[t=%6.1fs] synthetic sentFrames=%llu feedbackBytes=%llu "
                "feedbackFrames=%llu buttons=0x%08X dpad=0x%02X "
                "lx=%3d ly=%3d rx=%3d ry=%3d\n",
                elapsed,
                static_cast<unsigned long long>(sentFrames),
                static_cast<unsigned long long>(feedbackBytes),
                static_cast<unsigned long long>(feedbackFrames),
                buttons, dpad, lx, ly, rx, ry);
        }
        if (readback && now - lastReadback >= std::chrono::seconds(5)) {
            lastReadback = now;
            LogSyntheticReadback("periodic");
        }
    }

    std::printf("stopping synthetic...\n");
    DestroyPersona(state, stream);
    WSACleanup();
    std::printf("synthetic stopped; virtual DS5 removed\n");
    PrintVirtualDualSenseBounded();
    return 0;
}

namespace {

constexpr size_t kCaptureQueueMax = 8192;
constexpr size_t kReceiveBufferSize = 65536;
constexpr size_t kFeedbackObjectSize = 474;

struct CaptureRecord {
    int64_t timestampUs = 0;
    uint32_t generation = 0;
    uint8_t type = 0;
    uint32_t sequence = 0;
    std::vector<uint8_t> payload;
    bool marker = false;
    std::string markerText;
};

class CaptureQueue {
public:
    bool TryPush(CaptureRecord record)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (queue_.size() >= kCaptureQueueMax) {
            ++dropped_;
            return false;
        }
        queue_.push_back(std::move(record));
        condition_.notify_one();
        return true;
    }

    bool Pop(CaptureRecord& record)
    {
        std::unique_lock<std::mutex> lock(mutex_);
        condition_.wait(lock, [&] { return stop_ || !queue_.empty(); });
        if (queue_.empty()) {
            return false;
        }
        record = std::move(queue_.front());
        queue_.pop_front();
        return true;
    }

    void Stop()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        condition_.notify_all();
    }

    uint64_t Dropped() const { return dropped_.load(); }
    size_t Size() const
    {
        std::lock_guard<std::mutex> lock(mutex_);
        return queue_.size();
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable condition_;
    std::deque<CaptureRecord> queue_;
    bool stop_ = false;
    std::atomic<uint64_t> dropped_{0};
};

struct CaptureStats {
    std::atomic<uint64_t> frames81{0};
    std::atomic<uint64_t> frames83{0};
    std::atomic<uint64_t> frames84{0};
    std::atomic<int64_t> lastFrameUs{0};
    std::vector<uint32_t> firstSequence;
    std::vector<uint32_t> lastSequence;
    uint64_t rumbleChanges = 0;
    uint64_t triggerChanges = 0;
    uint64_t reportChanges = 0;
    uint64_t carrierChanges = 0;
    int64_t lastRumbleChangeUs = 0;
    int64_t lastTriggerChangeUs = 0;
    int64_t lastReportChangeUs = 0;
    int64_t lastCarrierChangeUs = 0;
};

std::string GetMarkerPath(const std::string& label)
{
    return ExpandEnv("%TEMP%\\vaderhaptics_capture_markers_" + label + ".txt");
}

std::string GetTraceTimestamp()
{
    SYSTEMTIME time = {};
    GetLocalTime(&time);
    char buffer[64] = {};
    std::snprintf(buffer, sizeof(buffer), "%04u%02u%02u_%02u%02u%02u",
        time.wYear, time.wMonth, time.wDay, time.wHour, time.wMinute,
        time.wSecond);
    return buffer;
}

void WriteSummaryFile(const std::string& outDir, const std::string& label,
    const std::string& summary)
{
    std::string path = outDir + "\\capture_" + label + "_summary.txt";
    std::ofstream file(path);
    if (file) {
        file << summary;
    }
    std::printf("summary written to %s\n", path.c_str());
}

bool RemovePersonaForReconnect(BridgeState& state)
{
    if (!state.busId.empty() && !state.devId.empty()) {
        std::string response;
        ApiRequest("bus/" + state.busId + "/remove " + state.devId, response);
        ApiRequest("bus/remove " + state.busId, response);
    }
    if (state.usbipPort >= 0) {
        RunUsbipDetach(state.usbipPort);
    }
    return true;
}

} // namespace

int RunCapture(int durationSeconds, const std::string& label,
    const std::string& outDir)
{
    SetConsoleCtrlHandler(ConsoleHandler, TRUE);

    std::string tracePath =
        outDir + "\\capture_" + label + "_" + GetTraceTimestamp() + ".bin";
    std::ofstream traceFile(tracePath, std::ios::binary);
    if (!traceFile) {
        std::printf("ERROR: cannot open trace file %s\n", tracePath.c_str());
        return 1;
    }
    if (!vaderhaptics::WriteTraceHeader(traceFile)) {
        std::printf("ERROR: cannot write VHTC trace header %s\n",
            tracePath.c_str());
        return 1;
    }

    HidDeviceInfo vader;
    HANDLE vaderHandle = INVALID_HANDLE_VALUE;
    VaderState initialVaderState;
    if (!OpenLiveVaderMi01(vader, vaderHandle, initialVaderState)) {
        std::printf("ERROR: no live Vader 5 Pro MI_01 candidate passed "
                    "GET_INFO/status/ACQUIRE/0xEF validation.\n");
        return 1;
    }

    WSADATA wsa = {};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::printf("ERROR: WSAStartup failed\n");
        CloseHandle(vaderHandle);
        return 1;
    }

    BridgeState state;
    if (!EnsureViiperServer(state)) {
        WSACleanup();
        CloseHandle(vaderHandle);
        return 1;
    }
    SOCKET stream = INVALID_SOCKET;
    if (!CreatePersona(state, stream)) {
        DestroyPersona(state, stream);
        WSACleanup();
        CloseHandle(vaderHandle);
        return 1;
    }
    DWORD streamTimeout = 500;
    setsockopt(stream, SOL_SOCKET, SO_RCVTIMEO,
        reinterpret_cast<const char*>(&streamTimeout), sizeof(streamTimeout));
    DWORD sendTimeout = 2000;
    setsockopt(stream, SOL_SOCKET, SO_SNDTIMEO,
        reinterpret_cast<const char*>(&sendTimeout), sizeof(sendTimeout));

    std::atomic<SOCKET> currentStream{stream};
    std::atomic<bool> reconnectRequested{false};
    std::atomic<bool> stopCapture{false};
    int reconnectAttempts = 0;

    CaptureQueue queue;
    CaptureStats stats;
    uint64_t markerRecordsQueued = 0;
    uint64_t markerRecordsDropped = 0;
    std::atomic<bool> traceWriterFailed{false};

    auto start = std::chrono::steady_clock::now();
    auto nowUs = [&]() -> int64_t {
        return std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - start).count();
    };
    auto enqueueMarker = [&](int64_t timestampUs, const std::string& text) {
        CaptureRecord record;
        record.timestampUs = timestampUs;
        record.marker = true;
        record.markerText = text;
        if (queue.TryPush(std::move(record))) {
            ++markerRecordsQueued;
        }
        else {
            ++markerRecordsDropped;
        }
    };

    vaderhaptics::VpcmParser parser(
        [&](const vaderhaptics::VpcmFrame& frame) {
            int64_t ts = nowUs();
            stats.lastFrameUs.store(ts);
            if (frame.type == 0x81) {
                ++stats.frames81;
            }
            else if (frame.type == 0x83) {
                ++stats.frames83;
            }
            else if (frame.type == 0x84) {
                ++stats.frames84;
            }
            size_t genIndex = static_cast<size_t>(frame.generation - 1);
            if (stats.firstSequence.size() <= genIndex) {
                stats.firstSequence.resize(genIndex + 1);
                stats.lastSequence.resize(genIndex + 1);
            }
            if (stats.firstSequence[genIndex] == 0) {
                stats.firstSequence[genIndex] = frame.sequence;
            }
            stats.lastSequence[genIndex] = frame.sequence;

            const uint8_t* feedback = frame.payload.data();
            size_t feedbackSize = frame.payload.size();
            if (frame.type == 0x83) {
                feedback += 2;
                feedbackSize -= 2;
            }
            static thread_local uint32_t lastGen = 0;
            static thread_local std::vector<uint8_t> lastFeedback;
            if (frame.generation != lastGen || lastFeedback.size() != feedbackSize) {
                lastGen = frame.generation;
                lastFeedback.assign(feedback, feedback + feedbackSize);
            }
            else {
                auto changed = [&](size_t begin, size_t end) {
                    return std::memcmp(feedback + begin, lastFeedback.data() + begin,
                               end - begin) != 0;
                };
                if (changed(0, 2)) {
                    ++stats.rumbleChanges;
                    stats.lastRumbleChangeUs = ts;
                }
                if (changed(6, 28)) {
                    ++stats.triggerChanges;
                    stats.lastTriggerChangeUs = ts;
                }
                if (changed(28, 76)) {
                    ++stats.reportChanges;
                    stats.lastReportChangeUs = ts;
                }
                if (feedbackSize == kFeedbackObjectSize && changed(76, 474)) {
                    ++stats.carrierChanges;
                    stats.lastCarrierChangeUs = ts;
                }
                lastFeedback.assign(feedback, feedback + feedbackSize);
            }

            CaptureRecord record;
            record.timestampUs = ts;
            record.generation = frame.generation;
            record.type = frame.type;
            record.sequence = frame.sequence;
            record.payload = frame.payload;
            queue.TryPush(std::move(record));
        },
        [](vaderhaptics::VpcmError) {
            // counted by the parser; no hot-path action needed
        });

    std::thread writerThread([&]() {
        CaptureRecord record;
        while (queue.Pop(record)) {
            vaderhaptics::CaptureTraceRecord traceRecord;
            traceRecord.timestampUs = record.timestampUs;
            traceRecord.generation = record.generation;
            traceRecord.type = record.type;
            traceRecord.sequence = record.sequence;
            if (record.marker) {
                traceRecord.kind = vaderhaptics::CaptureRecordKind::Marker;
                traceRecord.payload.assign(record.markerText.begin(),
                    record.markerText.end());
            }
            else {
                traceRecord.kind = vaderhaptics::CaptureRecordKind::Frame;
                traceRecord.payload = std::move(record.payload);
            }
            if (!vaderhaptics::AppendTraceRecord(traceFile, traceRecord)) {
                traceWriterFailed.store(true);
            }
        }
    });

    std::thread receiveThread([&]() {
        uint8_t buffer[kReceiveBufferSize];
        while (!stopCapture.load()) {
            SOCKET sock = currentStream.load();
            if (sock == INVALID_SOCKET) {
                Sleep(10);
                continue;
            }
            int received = recv(sock,
                reinterpret_cast<char*>(buffer), sizeof(buffer), 0);
            if (received > 0) {
                parser.Feed(buffer, static_cast<size_t>(received));
                continue;
            }
            if (received == 0) {
                // orderly close by peer: treat as stream failure
            }
            else {
                int error = WSAGetLastError();
                if (error == WSAETIMEDOUT || error == WSAEWOULDBLOCK) {
                    continue;
                }
            }
            if (currentStream.load() == sock) {
                std::printf("receive failure; resetting generation and "
                            "requesting reconnect\n");
                parser.ResetGeneration();
                reconnectRequested.store(true);
            }
        }
    });

    std::string markerPath = GetMarkerPath(label);
    std::ifstream markerFile(markerPath);
    size_t processedMarkers = 0;
    auto lastMarkerCheck = start;
    struct ScheduledMarker {
        int64_t deadlineUs = 0;
        std::string text;
        bool emitted = false;
    };
    std::vector<ScheduledMarker> scheduledMarkers;

    uint32_t inputSequence = 0;
    uint64_t sentFrames = 0;
    uint8_t lastPayload[33] = {};
    MapToDs5(initialVaderState, lastPayload);
    auto lastSend = start;
    auto lastDiag = start;
    vaderhaptics::CaptureInputMarkerDetector inputMarkerDetector;

    std::printf("capture started: label=%s duration=%ds trace=%s\n",
        label.c_str(), durationSeconds, tracePath.c_str());
    PrintVirtualDualSenseBounded();

    while (!g_interrupted && !stopCapture.load()) {
        auto now = std::chrono::steady_clock::now();
        double elapsed = std::chrono::duration<double>(now - start).count();
        if (durationSeconds > 0 && elapsed >= durationSeconds) {
            break;
        }

        if (reconnectRequested.load()) {
            std::printf("reconnecting source generation...\n");
            SOCKET old = currentStream.exchange(INVALID_SOCKET);
            if (old != INVALID_SOCKET) {
                closesocket(old);
            }
            RemovePersonaForReconnect(state);
            SOCKET newStream = INVALID_SOCKET;
            ++reconnectAttempts;
            bool ok = CreatePersona(state, newStream);
            if (ok) {
                DWORD rcvTimeout = 500;
                setsockopt(newStream, SOL_SOCKET, SO_RCVTIMEO,
                    reinterpret_cast<const char*>(&rcvTimeout),
                    sizeof(rcvTimeout));
                DWORD sndTimeout = 2000;
                setsockopt(newStream, SOL_SOCKET, SO_SNDTIMEO,
                    reinterpret_cast<const char*>(&sndTimeout),
                    sizeof(sndTimeout));
                currentStream.store(newStream);
                reconnectRequested.store(false);
                std::printf("reconnect ok (attempt %d), new generation %u\n",
                    reconnectAttempts, parser.generation());
            }
            else {
                std::printf("ERROR: reconnect failed (attempt %d)\n",
                    reconnectAttempts);
                stopCapture.store(true);
                break;
            }
            if (reconnectAttempts > 2) {
                std::printf("ERROR: too many reconnects; stopping capture\n");
                stopCapture.store(true);
                break;
            }
        }

        uint8_t report[64] = {};
        DWORD read = 0;
        bool haveReport = ReadHidWithTimeout(
            vaderHandle, report, sizeof(report), 4, &read);
        VaderState vaderState;
        bool decoded = haveReport && read > 0 &&
            DecodeVaderReport(report, read, vaderState);

        uint8_t payload[33] = {};
        bool stateChanged = false;
        int64_t inputTimestampUs = 0;
        vaderhaptics::CaptureInputMarkerDetector::Edges inputEdges;
        if (decoded) {
            // This timestamp shares the steady_clock domain used by the VPCM
            // receive callback. Edge classification only observes the decoded
            // state; marker queueing occurs after this input has been sent.
            inputTimestampUs = nowUs();
            vaderhaptics::MarkerInputState markerInput;
            markerInput.lx = vaderState.lx;
            markerInput.ly = vaderState.ly;
            markerInput.l2 = vaderState.l2;
            markerInput.r2 = vaderState.r2;
            markerInput.face = vaderState.face;
            inputEdges = inputMarkerDetector.Observe(markerInput);
            MapToDs5(vaderState, payload);
            stateChanged = std::memcmp(payload, lastPayload, 33) != 0;
            std::memcpy(lastPayload, payload, 33);
        }

        SOCKET sendSocket = currentStream.load();
        bool shouldSend = sendSocket != INVALID_SOCKET &&
            (decoded && (stateChanged || sentFrames == 0));
        if (!shouldSend && sendSocket != INVALID_SOCKET &&
            now - lastSend >= std::chrono::milliseconds(100)) {
            shouldSend = true;
        }
        if (shouldSend) {
            auto frame = BuildVpcmInputFrame(inputSequence++, lastPayload);
            int sent = send(sendSocket,
                reinterpret_cast<const char*>(frame.data()),
                static_cast<int>(frame.size()), 0);
            if (sent == static_cast<int>(frame.size())) {
                ++sentFrames;
                lastSend = now;
            }
            else {
                std::printf("input send failure (%d); requesting reconnect\n",
                    WSAGetLastError());
                parser.ResetGeneration();
                reconnectRequested.store(true);
            }
        }

        // Sparse diagnostic metadata is queued only after the current Vader
        // state has taken the normal forwarding path. No full-rate input
        // report or synchronous console I/O is added to that path.
        for (size_t index = 0; index < inputEdges.count; ++index) {
            enqueueMarker(inputTimestampUs, inputEdges.names[index]);
        }

        if (now - lastMarkerCheck >= std::chrono::milliseconds(200)) {
            lastMarkerCheck = now;
            std::vector<std::string> lines;
            std::string line;
            markerFile.clear();
            markerFile.seekg(0);
            while (std::getline(markerFile, line)) {
                std::string trimmed = Trim(line);
                if (!trimmed.empty()) {
                    lines.push_back(trimmed);
                }
            }
            if (lines.size() > processedMarkers) {
                for (size_t i = processedMarkers; i < lines.size(); ++i) {
                    std::string line = lines[i];
                    if (line.size() >= 3 &&
                        static_cast<uint8_t>(line[0]) == 0xEF &&
                        static_cast<uint8_t>(line[1]) == 0xBB &&
                        static_cast<uint8_t>(line[2]) == 0xBF) {
                        line.erase(0, 3);
                    }
                    line = Trim(line);
                    if (line.rfind("at ", 0) == 0) {
                        std::string rest = Trim(line.substr(3));
                        size_t space = rest.find(' ');
                        if (space != std::string::npos) {
                            double seconds = std::atof(
                                rest.substr(0, space).c_str());
                            ScheduledMarker scheduled;
                            scheduled.deadlineUs =
                                static_cast<int64_t>(seconds * 1000000.0);
                            scheduled.text = rest.substr(space + 1);
                            scheduledMarkers.push_back(std::move(scheduled));
                        }
                    }
                    else {
                        int64_t ts = nowUs();
                        enqueueMarker(ts, line);
                        std::printf("marker at t=%.3fs: %s\n",
                            static_cast<double>(ts) / 1000000.0,
                            line.c_str());
                    }
                }
                processedMarkers = lines.size();
            }

            for (auto& scheduled : scheduledMarkers) {
                if (!scheduled.emitted && nowUs() >= scheduled.deadlineUs) {
                    scheduled.emitted = true;
                    int64_t ts = nowUs();
                    enqueueMarker(ts, scheduled.text);
                    std::printf("scheduled marker at t=%.3fs: %s\n",
                        static_cast<double>(ts) / 1000000.0,
                        scheduled.text.c_str());
                }
            }
        }

        if (now - lastDiag >= std::chrono::seconds(5)) {
            lastDiag = now;
            // periodic console heartbeat, no disk I/O in this path
            std::printf("[capture t=%6.1fs] frames81=%llu frames83=%llu "
                        "queue=%zu dropped=%llu\n",
                elapsed,
                static_cast<unsigned long long>(stats.frames81.load()),
                static_cast<unsigned long long>(stats.frames83.load()),
                queue.Size(), static_cast<unsigned long long>(queue.Dropped()));
        }
    }

    std::printf("stopping capture...\n");
    stopCapture.store(true);
    reconnectRequested.store(false);
    if (receiveThread.joinable()) {
        receiveThread.join();
    }
    // The receive thread is the final producer. Stop the writer only after it
    // exits so no valid final feedback/marker record is silently stranded.
    queue.Stop();
    if (writerThread.joinable()) {
        writerThread.join();
    }

    double duration = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();

    SOCKET finalStream = currentStream.exchange(INVALID_SOCKET);
    if (finalStream != INVALID_SOCKET) {
        closesocket(finalStream);
    }
    DestroyPersona(state, INVALID_SOCKET);
    WSACleanup();
    CloseHandle(vaderHandle);

    std::string summary;
    summary += "label: " + label + "\n";
    summary += "duration_s: " + std::to_string(duration) + "\n";
    summary += "frames_0x81: " + std::to_string(stats.frames81.load()) + "\n";
    summary += "frames_0x83: " + std::to_string(stats.frames83.load()) + "\n";
    summary += "frames_0x84: " + std::to_string(stats.frames84.load()) + "\n";
    summary += "pcm_generations_observed: " +
        std::string(stats.frames83.load() > 0 ? "yes" : "no") + "\n";
    summary += "sequence_gaps: " +
        std::to_string(parser.errorCount(vaderhaptics::VpcmError::SequenceGap)) + "\n";
    summary += "crc_mismatches: " +
        std::to_string(parser.errorCount(vaderhaptics::VpcmError::CrcMismatch)) + "\n";
    summary += "malformed_magic: " +
        std::to_string(parser.errorCount(vaderhaptics::VpcmError::BadMagic)) + "\n";
    summary += "malformed_version: " +
        std::to_string(parser.errorCount(vaderhaptics::VpcmError::BadVersion)) + "\n";
    summary += "invalid_length: " +
        std::to_string(parser.errorCount(vaderhaptics::VpcmError::BadLength)) + "\n";
    summary += "unknown_type: " +
        std::to_string(parser.errorCount(vaderhaptics::VpcmError::UnknownType)) + "\n";
    summary += "writer_dropped_records: " +
        std::to_string(queue.Dropped()) + "\n";
    summary += "marker_records_queued: " +
        std::to_string(markerRecordsQueued) + "\n";
    summary += "marker_records_dropped: " +
        std::to_string(markerRecordsDropped) + "\n";
    summary += "trace_writer_failed: " +
        std::string(traceWriterFailed.load() ? "yes" : "no") + "\n";
    summary += "reconnect_attempts: " + std::to_string(reconnectAttempts) + "\n";
    summary += "input_frames_sent: " + std::to_string(sentFrames) + "\n";
    summary += "rumble_field_changes: " + std::to_string(stats.rumbleChanges) + "\n";
    summary += "trigger_block_changes: " + std::to_string(stats.triggerChanges) + "\n";
    summary += "raw_report_changes: " + std::to_string(stats.reportChanges) + "\n";
    summary += "carrier_changes: " + std::to_string(stats.carrierChanges) + "\n";

    for (size_t gen = 0; gen < stats.firstSequence.size(); ++gen) {
        summary += "generation_" + std::to_string(gen + 1) +
            "_first_seq: " + std::to_string(stats.firstSequence[gen]) +
            " last_seq: " + std::to_string(stats.lastSequence[gen]) + "\n";
    }
    const int64_t healthTimestampUs = static_cast<int64_t>(duration * 1000000.0);
    vaderhaptics::CaptureTraceRecord healthRecord;
    healthRecord.kind = vaderhaptics::CaptureRecordKind::Health;
    healthRecord.timestampUs = healthTimestampUs;
    healthRecord.payload.assign(summary.begin(), summary.end());
    if (!vaderhaptics::AppendTraceRecord(traceFile, healthRecord)) {
        traceWriterFailed.store(true);
    }
    traceFile.close();

    std::vector<vaderhaptics::CaptureTraceRecord> traceRecords;
    uint32_t analyzedTraceVersion = 0;
    std::string analysisError;
    if (vaderhaptics::ReadCaptureTrace(tracePath, traceRecords,
            analyzedTraceVersion, analysisError)) {
        const auto analysis = vaderhaptics::AnalyzeCaptureTrace(
            traceRecords, analyzedTraceVersion);
        summary += "\n--- marker-centered analysis ---\n";
        summary += vaderhaptics::FormatCaptureAnalysis(analysis);
    }
    else {
        summary += "\nmarker_centered_analysis_error: " + analysisError + "\n";
    }

    std::printf("--- capture summary ---\n%s", summary.c_str());
    WriteSummaryFile(outDir, label, summary);
    std::printf("capture stopped; trace=%s\n", tracePath.c_str());
    PrintVirtualDualSenseBounded();
    return 0;
}

int StopBridge()
{
    BridgeState state;
    const auto stateResult = ReadStateFile(state);
    if (stateResult ==
        vaderhaptics::runtime::SessionStateReadResult::Invalid) {
        std::printf("ERROR: refusing cleanup from invalid recovery state; "
                    "manual inspection required\n");
        return 1;
    }
    if (stateResult ==
        vaderhaptics::runtime::SessionStateReadResult::NotFound) {
        const int visibilityResult = VisibilityRestore();
        const bool ownershipReleased = ReleaseVaderOwnership();
        std::printf("no bridge state found; visibility recovery %s; "
                    "Vader ownership release %s\n",
            visibilityResult == 0 ? "complete" : "failed",
            ownershipReleased ? "complete" : "failed");
        return visibilityResult == 0 && ownershipReleased ? 0 : 1;
    }
    if (state.bridgePid != 0 && state.bridgePid != GetCurrentProcessId()) {
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE,
            static_cast<DWORD>(state.bridgePid));
        if (process) {
            HANDLE stopEvent = OpenEventA(
                EVENT_MODIFY_STATE, FALSE, kBridgeStopEventName);
            if (!stopEvent || !SetEvent(stopEvent)) {
                if (stopEvent) CloseHandle(stopEvent);
                CloseHandle(process);
                std::printf("ERROR: active bridge pid %lu could not be "
                            "signaled; no concurrent cleanup attempted\n",
                    static_cast<unsigned long>(state.bridgePid));
                return 1;
            }
            CloseHandle(stopEvent);
            std::printf("graceful stop signaled to bridge pid %lu\n",
                static_cast<unsigned long>(state.bridgePid));
            const DWORD wait = WaitForSingleObject(process, 30000);
            CloseHandle(process);
            if (wait == WAIT_OBJECT_0) {
                std::printf("active bridge completed its own restoration\n");
                return 0;
            }
            std::printf("ERROR: active bridge did not stop within 30 seconds; "
                        "no concurrent cleanup attempted\n");
            return 1;
        }
    }
    const bool personaRestored = CleanupBridge(state);
    const bool ownershipReleased = ReleaseVaderOwnership();
    const bool restored = personaRestored && ownershipReleased;
    std::printf("bridge stopped; native mode restoration %s\n",
        restored ? "PASS" : "FAIL");
    return restored ? 0 : 1;
}

const char* kHidHideCli =
    "C:\\Program Files\\Nefarius Software Solutions\\HidHide\\x64\\HidHideCLI.exe";

std::string GetVisibilityStatePath()
{
    return ExpandEnv("%TEMP%\\vaderhaptics_hidhide_state.txt");
}

std::string GetVisibilityBaselinePath()
{
    return ExpandEnv("%TEMP%\\vaderhaptics_hidhide_baseline.txt");
}

bool RunHidHideCli(const std::vector<std::string>& args, std::string& output)
{
    std::string commandLine = std::string("\"") + kHidHideCli + "\"";
    for (const auto& arg : args) {
        // HidHideCLI parses command keywords without quotes; device instance
        // paths contain no spaces, so arguments are appended unquoted.
        commandLine += " " + arg;
    }

    SECURITY_ATTRIBUTES security = {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;

    std::string outPath = ExpandEnv("%TEMP%\\vaderhaptics_hidhide_out.txt");
    std::string errPath = ExpandEnv("%TEMP%\\vaderhaptics_hidhide_err.txt");
    DeleteFileA(outPath.c_str());
    DeleteFileA(errPath.c_str());

    HANDLE outFile = CreateFileA(outPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    HANDLE errFile = CreateFileA(errPath.c_str(), FILE_APPEND_DATA,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &security, OPEN_ALWAYS,
        FILE_ATTRIBUTE_NORMAL, nullptr);
    if (outFile == INVALID_HANDLE_VALUE || errFile == INVALID_HANDLE_VALUE) {
        if (outFile != INVALID_HANDLE_VALUE) CloseHandle(outFile);
        if (errFile != INVALID_HANDLE_VALUE) CloseHandle(errFile);
        return false;
    }

    STARTUPINFOA startup = {};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdOutput = outFile;
    startup.hStdError = errFile;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);

    PROCESS_INFORMATION process = {};
    bool ok = CreateProcessA(nullptr, commandLine.data(), nullptr, nullptr,
        TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    CloseHandle(outFile);
    CloseHandle(errFile);
    if (!ok) {
        return false;
    }
    CloseHandle(process.hThread);
    const DWORD wait = WaitForSingleObject(process.hProcess, 15000);
    DWORD exitCode = ERROR_GEN_FAILURE;
    if (wait == WAIT_OBJECT_0) {
        GetExitCodeProcess(process.hProcess, &exitCode);
    }
    else {
        TerminateProcess(process.hProcess, 1);
    }
    CloseHandle(process.hProcess);

    std::ifstream file(outPath, std::ios::binary);
    output.assign(std::istreambuf_iterator<char>(file),
        std::istreambuf_iterator<char>());
    std::ifstream errorFile(errPath, std::ios::binary);
    const std::string errorText{
        std::istreambuf_iterator<char>(errorFile),
        std::istreambuf_iterator<char>()};
    if (!errorText.empty()) {
        output += errorText;
    }
    return wait == WAIT_OBJECT_0 && exitCode == 0;
}

std::string LowerAscii(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(),
        [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
    return value;
}

bool ContainsCaseInsensitive(const std::string& text, const std::string& value)
{
    return LowerAscii(text).find(LowerAscii(value)) != std::string::npos;
}

std::string VisibilityQuery(const char* command)
{
    std::string output;
    RunHidHideCli({command}, output);
    return output;
}

bool WriteVisibilityState(const std::vector<std::string>& added,
    const std::string& priorCloak, const std::string& priorInv)
{
    std::ofstream file(GetVisibilityStatePath());
    if (!file) {
        return false;
    }
    file << "priorCloak=" << priorCloak << "\n";
    file << "priorInv=" << priorInv << "\n";
    for (const auto& path : added) {
        file << "added=" << path << "\n";
    }
    return true;
}

bool ReadVisibilityState(std::vector<std::string>& added,
    std::string& priorCloak, std::string& priorInv)
{
    std::ifstream file(GetVisibilityStatePath());
    if (!file) {
        return false;
    }
    added.clear();
    priorCloak.clear();
    priorInv.clear();
    std::string line;
    while (std::getline(file, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) {
            continue;
        }
        std::string key = line.substr(0, eq);
        std::string value = line.substr(eq + 1);
        if (key == "added") added.push_back(value);
        else if (key == "priorCloak") priorCloak = value;
        else if (key == "priorInv") priorInv = value;
    }
    return true;
}

std::string CloakValue(const std::string& stateText)
{
    return stateText.find("--cloak-on") != std::string::npos ? "on" : "off";
}

std::string InvValue(const std::string& stateText)
{
    return stateText.find("--inv-on") != std::string::npos ? "on" : "off";
}

bool VerifyNativeVisibilityBaseline()
{
    std::string appText;
    std::string devText;
    std::string cloakText;
    std::string invText;
    bool queried = RunHidHideCli({"--app-list"}, appText) &&
        RunHidHideCli({"--dev-list"}, devText) &&
        RunHidHideCli({"--cloak-state"}, cloakText) &&
        RunHidHideCli({"--inv-state"}, invText);
    size_t appRegistrations = 0;
    for (size_t pos = 0;
         (pos = appText.find("--app-reg", pos)) != std::string::npos;
         pos += 9) {
        ++appRegistrations;
    }
    const bool cliOnlyApps = appRegistrations == 1 &&
        ContainsCaseInsensitive(appText, kHidHideCli);
    const bool emptyDevices =
        devText.find("--dev-hide") == std::string::npos;
    const bool noRecoveryState =
        GetFileAttributesA(GetVisibilityStatePath().c_str()) ==
        INVALID_FILE_ATTRIBUTES;
    const bool pass = queried && cliOnlyApps && emptyDevices &&
        CloakValue(cloakText) == "off" && InvValue(invText) == "off" &&
        noRecoveryState;
    std::printf("HIDHIDE_NATIVE_BASELINE result=%s cli_only_apps=%u "
                "empty_devices=%u cloak=%s inverse=%s state_absent=%u\n",
        pass ? "PASS" : "FAIL", cliOnlyApps ? 1u : 0u,
        emptyDevices ? 1u : 0u, CloakValue(cloakText).c_str(),
        InvValue(invText).c_str(), noRecoveryState ? 1u : 0u);
    return pass;
}

int VisibilityApply(const std::vector<std::string>& paths)
{
    if (paths.empty()) {
        std::printf("visibility apply: no device instance paths given\n");
        return 2;
    }
    if (GetFileAttributesA(kHidHideCli) == INVALID_FILE_ATTRIBUTES) {
        std::printf("ERROR: HidHide CLI is not installed at %s\n", kHidHideCli);
        return 1;
    }
    if (GetFileAttributesA(GetVisibilityStatePath().c_str()) !=
        INVALID_FILE_ATTRIBUTES) {
        std::printf("ERROR: an existing VaderHaptics HidHide recovery state "
                    "must be restored before apply\n");
        return 1;
    }
    std::string appText;
    std::string devText;
    std::string cloakText;
    std::string invText;
    if (!RunHidHideCli({"--app-list"}, appText) ||
        !RunHidHideCli({"--dev-list"}, devText) ||
        !RunHidHideCli({"--cloak-state"}, cloakText) ||
        !RunHidHideCli({"--inv-state"}, invText)) {
        std::printf("ERROR: could not query the complete HidHide baseline\n");
        return 1;
    }
    std::string priorCloak = CloakValue(cloakText);
    std::string priorInv = InvValue(invText);
    size_t appRegistrations = 0;
    for (size_t pos = 0;
         (pos = appText.find("--app-reg", pos)) != std::string::npos;
         pos += 9) {
        ++appRegistrations;
    }
    const bool cleanAppList = appRegistrations == 1 &&
        ContainsCaseInsensitive(appText, kHidHideCli);
    const bool cleanDeviceList =
        devText.find("--dev-hide") == std::string::npos;
    if (!cleanAppList || !cleanDeviceList || priorCloak != "off" ||
        priorInv != "off") {
        std::printf("ERROR: production HidHide baseline is not clean: "
                    "cli_only_apps=%u empty_devices=%u cloak=%s inverse=%s\n",
            cleanAppList ? 1u : 0u, cleanDeviceList ? 1u : 0u,
            priorCloak.c_str(), priorInv.c_str());
        return 1;
    }

    // Persist the exact recovery obligation before the first HidHide mutation.
    if (!WriteVisibilityState(paths, priorCloak, priorInv)) {
        std::printf("ERROR: could not write visibility recovery state file\n");
        return 1;
    }

    for (const auto& path : paths) {
        std::string output;
        if (!RunHidHideCli({"--dev-hide", path}, output)) {
            std::printf("ERROR: HidHide CLI failed for %s\n", path.c_str());
            VisibilityRestore();
            return 1;
        }
        std::printf("hidden: %s\n", path.c_str());
    }
    std::string output;
    if (!RunHidHideCli({"--cloak-on"}, output)) {
        std::printf("ERROR: HidHide cloak-on failed\n");
        VisibilityRestore();
        return 1;
    }
    if (!RunHidHideCli({"--dev-list"}, devText) ||
        !RunHidHideCli({"--cloak-state"}, cloakText) ||
        !RunHidHideCli({"--inv-state"}, invText)) {
        std::printf("ERROR: could not verify HidHide production state\n");
        VisibilityRestore();
        return 1;
    }
    bool targetsPresent = true;
    for (const auto& path : paths) {
        targetsPresent = targetsPresent &&
            ContainsCaseInsensitive(devText, path);
    }
    if (!targetsPresent || CloakValue(cloakText) != "on" ||
        InvValue(invText) != "off") {
        std::printf("ERROR: HidHide production-state verification failed\n");
        VisibilityRestore();
        return 1;
    }
    std::printf("visibility apply done; cloak on; priorCloak=%s priorInv=%s\n",
        priorCloak.c_str(), priorInv.c_str());
    return 0;
}

int VisibilityRestore()
{
    std::vector<std::string> added;
    std::string priorCloak;
    std::string priorInv;
    if (!ReadVisibilityState(added, priorCloak, priorInv)) {
        std::printf("visibility restore: no VaderHaptics state found; "
                    "nothing to restore\n");
        return 0;
    }
    bool ok = true;
    for (const auto& path : added) {
        std::string output;
        if (!RunHidHideCli({"--dev-unhide", path}, output)) {
            std::printf("ERROR: HidHide unhide failed for %s\n", path.c_str());
            ok = false;
        }
        else {
            std::printf("unhidden: %s\n", path.c_str());
        }
    }
    std::string output;
    if (priorCloak == "off") {
        ok = RunHidHideCli({"--cloak-off"}, output) && ok;
    }
    else {
        ok = RunHidHideCli({"--cloak-on"}, output) && ok;
    }
    if (priorInv == "on") {
        ok = RunHidHideCli({"--inv-on"}, output) && ok;
    }
    else {
        ok = RunHidHideCli({"--inv-off"}, output) && ok;
    }
    std::string devText;
    std::string cloakText;
    std::string invText;
    ok = RunHidHideCli({"--dev-list"}, devText) && ok;
    ok = RunHidHideCli({"--cloak-state"}, cloakText) && ok;
    ok = RunHidHideCli({"--inv-state"}, invText) && ok;
    for (const auto& path : added) {
        if (ContainsCaseInsensitive(devText, path)) {
            std::printf("ERROR: HidHide target remains hidden: %s\n",
                path.c_str());
            ok = false;
        }
    }
    if (CloakValue(cloakText) != priorCloak ||
        InvValue(invText) != priorInv) {
        std::printf("ERROR: HidHide cloak/inverse baseline was not restored\n");
        ok = false;
    }
    if (!ok) {
        std::printf("ERROR: HidHide restoration remains pending; state file "
                    "retained at %s\n", GetVisibilityStatePath().c_str());
        return 1;
    }
    DeleteFileA(GetVisibilityStatePath().c_str());
    std::printf("visibility restore done; cloak restored to %s\n",
        priorCloak.c_str());
    return 0;
}

int VisibilityStatus()
{
    std::printf("--- HidHide app-list ---\n%s",
        VisibilityQuery("--app-list").c_str());
    std::printf("--- HidHide dev-list ---\n%s",
        VisibilityQuery("--dev-list").c_str());
    std::printf("--- HidHide cloak-state ---\n%s",
        VisibilityQuery("--cloak-state").c_str());
    std::printf("--- HidHide inv-state ---\n%s",
        VisibilityQuery("--inv-state").c_str());

    std::vector<std::string> added;
    std::string priorCloak;
    std::string priorInv;
    if (ReadVisibilityState(added, priorCloak, priorInv)) {
        std::printf("--- VaderHaptics state file: PRESENT ---\n");
        std::printf("priorCloak=%s priorInv=%s\n", priorCloak.c_str(),
            priorInv.c_str());
        for (const auto& path : added) {
            std::printf("added=%s\n", path.c_str());
        }
        std::string devList = VisibilityQuery("--dev-list");
        for (const auto& path : added) {
            if (devList.find(path) != std::string::npos) {
                std::printf("stale check: %s still hidden\n", path.c_str());
            }
            else {
                std::printf("stale check: %s no longer hidden\n",
                    path.c_str());
            }
        }
    }
    else {
        std::printf("--- VaderHaptics state file: absent ---\n");
    }
    return 0;
}

int VisibilitySnapshot()
{
    std::ofstream file(GetVisibilityBaselinePath());
    if (!file) {
        std::printf("ERROR: could not write baseline file\n");
        return 1;
    }
    file << "app-list:\n" << VisibilityQuery("--app-list");
    file << "dev-list:\n" << VisibilityQuery("--dev-list");
    file << "cloak-state:\n" << VisibilityQuery("--cloak-state");
    file << "inv-state:\n" << VisibilityQuery("--inv-state");
    std::printf("visibility snapshot written to %s\n",
        GetVisibilityBaselinePath().c_str());
    return 0;
}
int StatusBridge()
{
    std::string version;
    if (PingViiper(version)) {
        std::printf("VIIPER server: running (%s)\n", version.c_str());
    }
    else {
        std::printf("VIIPER server: not running\n");
    }
    std::string response;
    if (ApiRequest("bus/list", response)) {
        std::printf("buses: %s\n", response.c_str());
    }
    auto devices = EnumerateHidDevices();
    bool vader = false;
    for (const auto& device : devices) {
        if (IsVader5Pro(device) && device.usagePage == 0xFFA0) {
            vader = true;
        }
    }
    std::printf("Vader MI_01 present: %s\n", vader ? "yes" : "no");
    return 0;
}

bool IsBridgeProcessActive(const BridgeState& state)
{
    if (state.bridgePid == 0) {
        return false;
    }
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE,
        static_cast<DWORD>(state.bridgePid));
    if (!process) {
        return false;
    }
    const bool active = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return active;
}

int RuntimeModeStatus()
{
    BridgeState state;
    const auto stateResult = ReadStateFile(state);
    const int virtualCount = CountVirtualDualSenseHidInterfaces();
    const bool visibilityRecoveryPending =
        GetFileAttributesA(GetVisibilityStatePath().c_str()) !=
        INVALID_FILE_ATTRIBUTES;

    const char* effective = "native";
    const char* health = "clean";
    if (stateResult ==
        vaderhaptics::runtime::SessionStateReadResult::Invalid) {
        effective = "unknown";
        health = "invalid-recovery-state";
    }
    else if (stateResult ==
        vaderhaptics::runtime::SessionStateReadResult::Loaded) {
        if (IsBridgeProcessActive(state)) {
            effective = "ps5";
            health = "active";
        }
        else {
            effective = "unknown";
            health = "recovery-required";
        }
    }
    else if (visibilityRecoveryPending || virtualCount != 0) {
        effective = "unknown";
        health = "recovery-required";
    }

    std::printf("RUNTIME_MODE effective=%s health=%s "
                "session_state=%s visibility_recovery=%u virtual_ds5=%d\n",
        effective, health,
        stateResult == vaderhaptics::runtime::SessionStateReadResult::Loaded
            ? "present"
            : stateResult ==
                vaderhaptics::runtime::SessionStateReadResult::Invalid
                ? "invalid"
                : "absent",
        visibilityRecoveryPending ? 1u : 0u, virtualCount);
    std::printf("Xbox Enhanced: reserved / unavailable\n");
    const auto capabilities =
        vaderhaptics::devices::Vader5ProCapabilities();
    std::printf("DEVICE_PROFILE id=%s four_lane_vibration=%u "
                "adaptive_trigger_resistance=%u\n",
        capabilities.deviceProfileId.c_str(),
        vaderhaptics::devices::HasCompleteFourLaneVibration(capabilities)
            ? 1u : 0u,
        capabilities.adaptiveTriggerResistance ? 1u : 0u);
    return std::strcmp(health, "invalid-recovery-state") == 0 ? 1 : 0;
}

int VerifyAndReportNativeMode()
{
    const bool xinputRestored = WaitForFreshProcessXInputCount(1);
    const int virtualCount = CountVirtualDualSenseHidInterfaces();
    const bool visibilityBaseline = VerifyNativeVisibilityBaseline();
    BridgeState staleState;
    const auto stateResult = ReadStateFile(staleState);
    const bool noSessionState = stateResult ==
        vaderhaptics::runtime::SessionStateReadResult::NotFound;
    const bool pass = xinputRestored && virtualCount == 0 &&
        visibilityBaseline && noSessionState;
    std::printf("MODE_NATIVE result=%s xinput=%u "
                "virtual_ds5_interfaces=%d visibility=%u session_state=%s\n",
        pass ? "PASS" : "FAIL", xinputRestored ? 1u : 0u,
        virtualCount, visibilityBaseline ? 1u : 0u,
        noSessionState ? "absent" : "present-or-invalid");
    return pass ? 0 : 1;
}

int EnterNativeMode()
{
    const int stopResult = StopBridge();
    if (stopResult != 0) {
        std::printf("MODE_NATIVE result=FAIL reason=cleanup-failed\n");
        return stopResult;
    }
    return VerifyAndReportNativeMode();
}

int RunDualSenseCommand(int duration, const std::string& configPath,
    bool motorOutputEnabled, const std::string& axisCsvPath,
    InputSourceKind inputSourceKind, const std::string& tracePath,
    const std::string& flydigiOnboardDirectory)
{
    g_bridgeStopEvent = CreateEventA(
        nullptr, TRUE, FALSE, kBridgeStopEventName);
    if (!g_bridgeStopEvent) {
        std::printf("ERROR: could not create graceful-stop event (%lu)\n",
            GetLastError());
        return 1;
    }
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        std::printf("ERROR: another VaderHaptics bridge run is active\n");
        CloseHandle(g_bridgeStopEvent);
        g_bridgeStopEvent = nullptr;
        return 1;
    }
    const int result = RunBridge(duration,
        ResolveM3ConfigPath(configPath), motorOutputEnabled,
        axisCsvPath, inputSourceKind, tracePath, flydigiOnboardDirectory);
    CloseHandle(g_bridgeStopEvent);
    g_bridgeStopEvent = nullptr;
    return result;
}

void PrintModeDecision(const vaderhaptics::runtime::ModeDecision& decision)
{
    std::printf("MODE_DECISION requested=%s effective=%s available=%u "
                "reason=%s\n",
        vaderhaptics::runtime::RuntimeModeId(decision.requested),
        vaderhaptics::runtime::RuntimeModeId(decision.effective),
        decision.requestAvailable ? 1u : 0u, decision.reason.c_str());
}

int TopologyStatus()
{
    const auto present = ResolveCurrentVaderMi00Instances(true);
    std::printf("present Vader 37D7:2401 MI_00 instances: %zu\n",
        present.size());
    for (const auto& instance : present) {
        bool started = false;
        const bool queried = IsDeviceStarted(instance, started);
        std::printf("  %s state=%s\n", NarrowAscii(instance).c_str(),
            queried ? (started ? "started" : "stopped") : "query-failed");
        const auto children = ResolveDirectGameChildren(instance);
        std::printf("    direct IG game children: %zu\n", children.size());
        for (const auto& child : children) {
            const bool childQueried = IsDeviceStarted(child, started);
            std::printf("      %s state=%s\n", NarrowAscii(child).c_str(),
                childQueried ? (started ? "started" : "stopped") :
                    "query-failed");
        }
    }
    std::printf("virtual DualSense HID interfaces: %d\n",
        CountVirtualDualSenseHidInterfaces());
    std::printf("connected XInput slots: %d\n", CountConnectedXInputSlots());
    return present.size() == 1 ? 0 : 1;
}
} // namespace

int main(int argc, char** argv)
{
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    if (g_viiperExe.empty()) {
        wchar_t executable[32768] = {};
        GetModuleFileNameW(nullptr, executable, static_cast<DWORD>(std::size(executable)));
        g_viiperExe = (std::filesystem::path(executable).parent_path() / L"runtime" / L"viiper.exe").string();
    }
    std::string command = argc > 1 ? argv[1] : "run";
    if (command == "startup-selftest") return RunViiperStartupSelfTest();
    if (command == "mi01-read-selftest") return RunMi01ReadSelfTest();
    int duration = 0;
    std::string configPath;
    std::string commandsPath;
    std::string axisCsvPath;
    std::string tracePath;
    std::string runtimeProfilePath;
    std::string flydigiOnboardDirectory;
    InputSourceKind inputSourceKind = InputSourceKind::Mi01;
    bool motorOutputEnabled = true;
    bool readback = false;
    bool resolveOnly = false;
    std::string label = "capture";
    std::string outDir = ExpandEnv("%TEMP%\\vaderhaptics_traces");
    int preWindowMs = 250;
    int postWindowMs = 1000;
    for (int i = 2; i < argc; ++i) {
        if (std::strcmp(argv[i], "--duration") == 0 && i + 1 < argc) {
            duration = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--config") == 0 && i + 1 < argc) {
            configPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--nodelay") == 0) {
            g_nodelay = true;
        }
        else if (std::strcmp(argv[i], "--input-diagnostics") == 0) {
            g_inputDiagnostics = true;
        }
        else if (std::strcmp(argv[i], "--motor-output") == 0 && i + 1 < argc) {
            const char* value = argv[++i];
            if (std::strcmp(value, "on") == 0) {
                motorOutputEnabled = true;
            }
            else if (std::strcmp(value, "off") == 0) {
                motorOutputEnabled = false;
            }
            else {
                std::printf("ERROR: --motor-output must be on or off\n");
                return 2;
            }
        }
        else if (std::strcmp(argv[i], "--viiper-exe") == 0 &&
            i + 1 < argc) {
            g_viiperExe = argv[++i];
        }
        else if (std::strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
            label = argv[++i];
        }
        else if (std::strcmp(argv[i], "--out-dir") == 0 && i + 1 < argc) {
            outDir = argv[++i];
        }
        else if (std::strcmp(argv[i], "--commands") == 0 && i + 1 < argc) {
            commandsPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--axis-csv") == 0 && i + 1 < argc) {
            axisCsvPath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--trace-out") == 0 &&
            i + 1 < argc) {
            tracePath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--profile") == 0 &&
            i + 1 < argc) {
            runtimeProfilePath = argv[++i];
        }
        else if (std::strcmp(argv[i], "--flydigi-onboard-dir") == 0 &&
            i + 1 < argc) {
            flydigiOnboardDirectory = argv[++i];
        }
        else if (std::strcmp(argv[i], "--input-source") == 0 &&
            i + 1 < argc) {
            const char* value = argv[++i];
            if (std::strcmp(value, "xinput") == 0) {
                inputSourceKind = InputSourceKind::XInput;
            }
            else if (std::strcmp(value, "mi01") == 0) {
                inputSourceKind = InputSourceKind::Mi01;
            }
            else {
                std::printf("ERROR: --input-source must be xinput or mi01\n");
                return 2;
            }
        }
        else if (std::strcmp(argv[i], "--readback") == 0) {
            readback = true;
        }
        else if (std::strcmp(argv[i], "--resolve-only") == 0) {
            resolveOnly = true;
        }
        else if (std::strcmp(argv[i], "--pre-ms") == 0 && i + 1 < argc) {
            preWindowMs = std::atoi(argv[++i]);
        }
        else if (std::strcmp(argv[i], "--post-ms") == 0 && i + 1 < argc) {
            postWindowMs = std::atoi(argv[++i]);
        }
    }

    if (command == "run") {
        return RunDualSenseCommand(duration, configPath, motorOutputEnabled,
            axisCsvPath, inputSourceKind, tracePath,
            flydigiOnboardDirectory);
    }
    if (command == "mode") {
        const std::string action = argc > 2 ? argv[2] : "status";
        if (action == "status") {
            return RuntimeModeStatus();
        }
        if (action == "native" || action == "recover") {
            if (resolveOnly) {
                vaderhaptics::runtime::ModeAvailability availability;
                const auto decision =
                    vaderhaptics::runtime::ResolveRuntimeMode(
                        vaderhaptics::runtime::RuntimeMode::Native,
                        availability);
                PrintModeDecision(decision);
                return 0;
            }
            return EnterNativeMode();
        }
        if (action == "ps5" || action == "dualsense") {
            vaderhaptics::runtime::ModeAvailability availability;
            const auto decision = vaderhaptics::runtime::ResolveRuntimeMode(
                vaderhaptics::runtime::RuntimeMode::DualSense, availability);
            PrintModeDecision(decision);
            if (resolveOnly) {
                return decision.requestAvailable ? 0 : 3;
            }
            return RunDualSenseCommand(duration, configPath,
                motorOutputEnabled, axisCsvPath, inputSourceKind, tracePath,
                flydigiOnboardDirectory);
        }
        if (action == "xbox" || action == "xbox-enhanced") {
            vaderhaptics::runtime::ModeAvailability availability;
            const auto decision = vaderhaptics::runtime::ResolveRuntimeMode(
                vaderhaptics::runtime::RuntimeMode::XboxEnhanced,
                availability);
            PrintModeDecision(decision);
            return 3;
        }
        if (action == "auto") {
            vaderhaptics::profiles::GameRuntimeProfile profile;
            if (!runtimeProfilePath.empty()) {
                std::string error;
                if (!vaderhaptics::profiles::LoadGameRuntimeProfile(
                        runtimeProfilePath, profile, error)) {
                    std::printf("ERROR: %s\n", error.c_str());
                    return 2;
                }
                std::printf("PROFILE executable=%s mode=%s preset=%s "
                            "restart_required=%u\n",
                    profile.executable.c_str(),
                    vaderhaptics::runtime::RuntimeModeId(
                        profile.requestedMode),
                    profile.tuningPreset.c_str(),
                    profile.restartRequired ? 1u : 0u);
            }
            const auto availability =
                vaderhaptics::profiles::AvailabilityForProfile(profile);
            const auto decision = vaderhaptics::runtime::ResolveRuntimeMode(
                profile.requestedMode, availability);
            PrintModeDecision(decision);
            if (resolveOnly) {
                return decision.requestAvailable ? 0 : 3;
            }
            if (!decision.requestAvailable) {
                return 3;
            }
            if (decision.effective ==
                vaderhaptics::runtime::RuntimeMode::DualSense) {
                return RunDualSenseCommand(duration, configPath,
                    motorOutputEnabled, axisCsvPath, inputSourceKind,
                    tracePath, flydigiOnboardDirectory);
            }
            return EnterNativeMode();
        }
        std::printf("usage: vader_ds5_bridge mode "
                    "status|native|ps5|auto|recover|xbox "
                    "[--profile PATH] [--resolve-only] [run options]\n");
        return 2;
    }
    if (command == "synthetic") {
        return RunSynthetic(duration, commandsPath, readback);
    }
    if (command == "capture") {
        return RunCapture(duration, label, outDir);
    }
    if (command == "analyze") {
        if (argc < 3 || argv[2][0] == '-') {
            std::printf("usage: vader_ds5_bridge analyze <trace.bin> "
                        "[--pre-ms N] [--post-ms N]\n");
            return 2;
        }
        if (preWindowMs < 0 || postWindowMs < 0) {
            std::printf("ERROR: analysis windows must be non-negative\n");
            return 2;
        }
        std::vector<vaderhaptics::CaptureTraceRecord> traceRecords;
        uint32_t traceVersion = 0;
        std::string error;
        if (!vaderhaptics::ReadCaptureTrace(argv[2], traceRecords,
                traceVersion, error)) {
            std::printf("ERROR: trace analysis failed: %s\n", error.c_str());
            return 1;
        }
        vaderhaptics::CaptureAnalysisOptions options;
        options.preWindowUs = static_cast<int64_t>(preWindowMs) * 1000;
        options.postWindowUs = static_cast<int64_t>(postWindowMs) * 1000;
        const auto analysis = vaderhaptics::AnalyzeCaptureTrace(
            traceRecords, traceVersion, options);
        std::printf("%s", vaderhaptics::FormatCaptureAnalysis(analysis).c_str());
        return 0;
    }
    if (command == "input-selftest") {
        return RunInputMappingSelfTest();
    }
    if (command == "xinput-count") {
        const int count = CountConnectedXInputSlots();
        std::printf("connected XInput slots: %d\n", count);
        return count <= static_cast<int>(XUSER_MAX_COUNT) ? count : 255;
    }
    if (command == "visibility") {
        std::string sub = argc > 2 ? argv[2] : "status";
        if (sub == "apply") {
            std::vector<std::string> paths;
            for (int i = 3; i < argc; ++i) {
                paths.emplace_back(argv[i]);
            }
            return VisibilityApply(paths);
        }
        if (sub == "restore") {
            return VisibilityRestore();
        }
        if (sub == "status") {
            return VisibilityStatus();
        }
        if (sub == "snapshot") {
            return VisibilitySnapshot();
        }
        std::printf("usage: vader_ds5_bridge visibility "
                    "apply <path> [path...] | restore | status | snapshot\n");
        return 2;
    }
    if (command == "stop") {
        return StopBridge();
    }
    if (command == "status") {
        return StatusBridge();
    }
    if (command == "topology-status") {
        return TopologyStatus();
    }
    std::printf(
        "usage: vader_ds5_bridge run [--duration N] [--config PATH] "
        "[--nodelay] [--motor-output on|off] [--viiper-exe PATH] "
        "[--axis-csv PATH] [--trace-out PATH] "
        "[--flydigi-onboard-dir PATH] "
        "[--input-source xinput|mi01] | "
        "mode status|native|ps5|auto|recover|xbox "
        "[--profile PATH] [--resolve-only] [run options] | "
        "synthetic [--duration N] [--commands FILE] [--readback] "
        "[--nodelay] [--viiper-exe PATH] | "
        "capture [--duration N] [--label NAME] [--out-dir DIR] | "
        "analyze <trace.bin> [--pre-ms N] [--post-ms N] | "
        "input-selftest | topology-status | stop | status\n");
    return 2;
}
