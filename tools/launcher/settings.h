#pragma once

#include <filesystem>

namespace launcher {
bool IsSpaceStationDirectory(const std::filesystem::path& directory);
std::filesystem::path ReadSpaceStationDirectory(const std::filesystem::path& ini);
bool SaveSpaceStationDirectory(const std::filesystem::path& ini,
    const std::filesystem::path& directory);
}
