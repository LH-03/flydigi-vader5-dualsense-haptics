#pragma once
#include <string>
#include <string_view>

namespace launcher {
bool BridgeStartupReady(std::string_view log);
std::wstring ExplainBridgeFailure(std::string_view log);
}
