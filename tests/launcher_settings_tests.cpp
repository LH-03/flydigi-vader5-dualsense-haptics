#include "settings.h"
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
