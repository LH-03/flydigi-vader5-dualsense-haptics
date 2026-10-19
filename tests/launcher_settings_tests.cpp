#include "settings.h"
#include "diagnostics.h"
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <iostream>

int main()
{
    const auto root = std::filesystem::temp_directory_path() /
        (L"vader-launcher-settings-" + std::to_wstring(GetCurrentProcessId()));
    const auto station = root / L"中文 空间站";
    std::filesystem::create_directories(station);
    std::ofstream(station / L"SpaceStationService.exe").put(' ');
    std::ofstream(station / L"Flydigi Space Station.exe").put(' ');
    const auto ini = root / L"VaderHapticsLauncher.ini";
    int failures = 0;
    auto check = [&](bool ok, const char* name) {
        std::cout << (ok ? "PASS " : "FAIL ") << name << '\n';
        if (!ok) ++failures;
    };
    const std::string denied = "GET_STATUS ok: rawTransport=0 thirdPartyAllowed=0\n"
        "reject: raw transport enable did not survive status readback\n"
        "ERROR: no live Vader 5 Pro MI_01 candidate passed validation.\n";
    check(launcher::ExplainBridgeFailure(denied).find(L"允许第三方应用接管映射") != std::wstring::npos,
        "permission failure gives exact setting and action");
    check(!launcher::BridgeStartupReady(denied), "rejected startup is not ready");
    check(!launcher::BridgeStartupReady("MODE_DECISION requested=ps5"), "process creation is not readiness");
    const std::string ready = "initial live input frame sent: yes\nM3 motor output: on\n";
    check(launcher::BridgeStartupReady(ready), "input and renderer startup required");
    check(!launcher::BridgeStartupReady(ready + "ERROR: stream send failed\n"), "startup error prevents success");
    check(launcher::ExplainBridgeFailure("ERROR: unfamiliar error\n").find(L"unfamiliar error") != std::wstring::npos,
        "unknown errors remain visible");
    check(launcher::ExplainBridgeFailure("ERROR: could not start VIIPER server\n").find(L"查看诊断") != std::wstring::npos,
        "VIIPER failure points to in-app diagnostics");
    const std::string serviceError = "INFO secret-do-not-copy\nVIIPER: error: listen tcp :3241: bind: forbidden by its access permissions.\n";
    check(launcher::ExplainBridgeFailure("ERROR: could not start VIIPER server\n", serviceError).find(L"通信端口") != std::wstring::npos,
        "port bind failure translated for users");
    const auto report = launcher::DiagnosticDetails("ERROR: could not start VIIPER server\n", serviceError, "MODE_NATIVE result=PASS\n");
    check(launcher::ExplainBridgeFailure("ERROR: ordinary input source 'mi01' produced no sample for 2012737 us; sustained outage, stopping safely\n").find(L"连续两秒") != std::wstring::npos,
        "runtime input outage explained separately from startup failure");
    check(report.find(L"secret-do-not-copy") == std::wstring::npos && report.find(L"bind:") != std::wstring::npos
        && report.find(L"MODE_NATIVE result=PASS") != std::wstring::npos,
        "copy report retains failure and recovery without service INFO secrets");
    const auto inputReport = launcher::DiagnosticDetails(
        "ERROR: ordinary input source disconnected;\n"
        "MI01_READ_SUMMARY completed=20 last_read_error=1167\n"
        "INPUT_LOOP_SUMMARY max_loop_gap_us=4100 max_poll_us=4000\n"
        "INPUT_GAP elapsed_ms=22000 duration_us=2008890 recovered=0\n", {}, {});
    check(inputReport.find(L"last_read_error=1167") != std::wstring::npos
        && inputReport.find(L"max_loop_gap_us=4100") != std::wstring::npos
        && inputReport.find(L"duration_us=2008890") != std::wstring::npos,
        "in-app diagnostics retain input read and timing evidence");
    check(launcher::ReadSpaceStationDirectory(ini).empty(), "missing configuration");
    check(!launcher::SaveSpaceStationDirectory(ini, root), "reject wrong folder");
    check(launcher::SaveSpaceStationDirectory(ini, station), "save new Chinese path");
    check(launcher::ReadSpaceStationDirectory(ini) == station, "read Chinese path");
    {
        std::ofstream legacy(ini, std::ios::binary | std::ios::trunc);
        legacy << "; UTF-8 comment: 中文\r\n[extra]\r\nkeep=yes\r\n"
                  "[paths]\r\nspace_station_dir=old\r\n";
    }
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
    check(launcher::SaveSpaceStationDirectory(ini, station), "migrate existing UTF-8 configuration");
    check(launcher::ReadSpaceStationDirectory(ini) == station, "read migrated path");
    wchar_t value[32] = {};
    GetPrivateProfileStringW(L"extra", L"keep", L"", value, 32, ini.c_str());
    check(std::wstring(value) == L"yes", "preserve other settings");
    check(launcher::SaveSpaceStationDirectory(ini, station), "save existing UTF-16 configuration");
    check(!launcher::SaveSpaceStationDirectory(ini, root / L"missing")
        && launcher::ReadSpaceStationDirectory(ini) == station, "invalid selection preserves saved path");
    std::filesystem::remove(station / L"SpaceStationService.exe");
    check(launcher::ReadSpaceStationDirectory(ini).empty(), "detect removed installation");
    std::filesystem::remove_all(root);
    return failures ? 1 : 0;
}
