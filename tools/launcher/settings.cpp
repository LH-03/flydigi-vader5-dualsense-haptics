#include "settings.h"
#include <windows.h>
#include <fstream>
#include <iterator>
#include <string>

namespace launcher {
bool IsSpaceStationDirectory(const std::filesystem::path& directory)
{
    if (directory.empty()) return false;
    std::error_code error;
    return std::filesystem::is_regular_file(directory / L"SpaceStationService.exe", error)
        && std::filesystem::is_regular_file(directory / L"Flydigi Space Station.exe", error);
}

std::filesystem::path ReadSpaceStationDirectory(const std::filesystem::path& ini)
{
    wchar_t value[32768] = {};
    GetPrivateProfileStringW(L"paths", L"space_station_dir", L"", value,
        static_cast<DWORD>(std::size(value)), ini.c_str());
    return IsSpaceStationDirectory(value) ? std::filesystem::path(value)
                                         : std::filesystem::path{};
}

bool SaveSpaceStationDirectory(const std::filesystem::path& ini,
    const std::filesystem::path& directory)
{
    if (!IsSpaceStationDirectory(directory)) return false;
    // Preserve other settings and comments, converting legacy ANSI/UTF-8 files
    // to UTF-16 so the Windows profile API round-trips Chinese paths reliably.
    std::wstring contents;
    std::error_code error;
    if (std::filesystem::exists(ini, error)) {
        std::ifstream input(ini, std::ios::binary);
        if (!input) return false;
        const std::string bytes{std::istreambuf_iterator<char>(input), {}};
        if (input.bad()) return false;
        if (bytes.size() >= 2 && static_cast<unsigned char>(bytes[0]) == 0xff
            && static_cast<unsigned char>(bytes[1]) == 0xfe) {
            if (bytes.size() % 2 != 0) return false;
            contents.resize((bytes.size() - 2) / sizeof(wchar_t));
            if (!contents.empty()) memcpy(contents.data(), bytes.data() + 2,
                bytes.size() - 2);
        }
        else if (!bytes.empty()) {
            const bool bom = bytes.starts_with("\xef\xbb\xbf");
            const char* data = bytes.data() + (bom ? 3 : 0);
            const int length = static_cast<int>(bytes.size() - (bom ? 3 : 0));
            UINT encoding = CP_UTF8;
            int count = MultiByteToWideChar(encoding, MB_ERR_INVALID_CHARS, data, length, nullptr, 0);
            if (!count && length) {
                encoding = CP_ACP;
                count = MultiByteToWideChar(encoding, 0, data, length, nullptr, 0);
            }
            if (!count && length) return false;
            contents.resize(count);
            if (count) MultiByteToWideChar(encoding, 0, data, length, contents.data(), count);
        }
    }
    else if (error) return false;

    auto temporary = ini;
    temporary += L".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        const wchar_t bom = 0xfeff;
        output.write(reinterpret_cast<const char*>(&bom), sizeof(bom));
        output.write(reinterpret_cast<const char*>(contents.data()),
            static_cast<std::streamsize>(contents.size() * sizeof(wchar_t)));
        output.close();
        if (!output) {
            std::filesystem::remove(temporary, error);
            return false;
        }
    }
    bool saved = WritePrivateProfileStringW(L"paths", L"space_station_dir",
        directory.c_str(), temporary.c_str()) != FALSE;
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, temporary.c_str());
    if (saved) saved = MoveFileExW(temporary.c_str(), ini.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!saved) std::filesystem::remove(temporary, error);
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, ini.c_str());
    return saved;
}
}
