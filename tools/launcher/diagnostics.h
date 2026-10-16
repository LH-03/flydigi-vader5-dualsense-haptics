#pragma once
#include <string>
#include <string_view>

namespace launcher {
bool BridgeStartupReady(std::string_view log);
std::wstring ExplainBridgeFailure(std::string_view log, std::string_view serviceLog = {});
std::wstring DiagnosticDetails(std::string_view bridge, std::string_view service,
    std::string_view recovery);
}
