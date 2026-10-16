#include "diagnostics.h"
#include <windows.h>

namespace launcher {
bool BridgeStartupReady(std::string_view log)
{
    return log.find("initial live input frame sent: yes") != log.npos
        && log.find("M3 motor output: on") != log.npos
        && log.find("ERROR:") == log.npos;
}

std::wstring ExplainBridgeFailure(std::string_view log)
{
    if (log.find("thirdPartyAllowed=0") != log.npos
        && log.find("reject: raw transport enable") != log.npos
        && log.find("M3 motor output:") == log.npos) {
        return L"手柄未允许第三方接管。点击下方“打开飞智设置”，在飞智空间站设置中"
            L"开启“允许第三方应用接管映射”，再回这里启动增强。若设置被锁定，请先正常退出 Steam。";
    }
    if (log.find("ordinary input source") != log.npos
        && log.find("disconnected;") != log.npos) {
        return L"手柄输入连接中断。请确认 USB 或 2.4G 接收器连接正常，再启动增强。";
    }
    if (log.find("ERROR: VIIPER feedback stream failed") != log.npos) {
        return L"虚拟手柄反馈连接中断。请查看 VIIPER 是否正常运行，再启动增强；详细记录见“打开运行日志”。";
    }
    const auto error = log.rfind("ERROR:");
    if (error != log.npos) {
        const auto end = log.find_first_of("\r\n", error);
        const auto line = log.substr(error, end == log.npos ? log.npos : end - error);
        const int size = MultiByteToWideChar(CP_UTF8, 0, line.data(),
            static_cast<int>(line.size()), nullptr, 0);
        std::wstring text(size, L'\0');
        if (size) MultiByteToWideChar(CP_UTF8, 0, line.data(),
            static_cast<int>(line.size()), text.data(), size);
        return L"桥接报告：" + text + L"\r\n点击“打开运行日志”查看 bridge.log；恢复记录在 native.log。";
    }
    return L"桥接停止或未完成启动，日志未提供明确错误。点击“打开运行日志”查看 bridge.log 和 native.log。";
}
}
