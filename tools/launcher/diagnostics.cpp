#include "diagnostics.h"
#include <windows.h>

namespace launcher {
namespace {
std::wstring Wide(std::string_view text)
{
    const int size = MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), nullptr, 0);
    std::wstring result(size, L'\0');
    if (size) MultiByteToWideChar(CP_UTF8, 0, text.data(),
        static_cast<int>(text.size()), result.data(), size);
    return result;
}

std::wstring RelevantLines(std::string_view log)
{
    std::wstring result;
    while (!log.empty()) {
        const auto end = log.find('\n');
        const auto line = log.substr(0, end);
        // Service INFO logs can contain an API password on first launch.
        // Include only failure/recovery evidence in the UI and clipboard.
        if (line.find("ERROR:") != line.npos || line.find("error:") != line.npos ||
            line.find(" ERROR ") != line.npos || line.find("reject:") != line.npos ||
            line.find("MI01_READ_SUMMARY ") != line.npos ||
            line.find("INPUT_LOOP_SUMMARY ") != line.npos ||
            line.find("INPUT_GAP ") != line.npos ||
            line.find("MODE_NATIVE result=") != line.npos) {
            result += Wide(line) + L"\r\n";
        }
        if (end == log.npos) break;
        log.remove_prefix(end + 1);
    }
    return result.empty() ? L"未记录到明确错误。\r\n" : result;
}
}
bool BridgeStartupReady(std::string_view log)
{
    return log.find("initial live input frame sent: yes") != log.npos
        && log.find("M3 motor output: on") != log.npos
        && log.find("ERROR:") == log.npos;
}

std::wstring ExplainBridgeFailure(std::string_view log, std::string_view serviceLog)
{
    if (log.find("VIIPER input queue fix is missing") != log.npos) {
        return L"当前虚拟手柄运行时缺少按键队列修复，可能漏掉快速按下/松开。增强已停止，请安装配套的输入修复版运行时后再试。";
    }
    if (log.find("ERROR: could not start VIIPER server") != log.npos) {
        if (serviceLog.find("bind:") != serviceLog.npos) {
            return L"虚拟手柄服务的通信端口被系统限制或其他程序占用。"
                L"手柄设置正常，无需再改接管开关。可先使用原生模式；点击“查看诊断”可直接查看并复制错误信息。";
        }
        return L"虚拟手柄服务启动失败，手柄接管开关无需再改。"
            L"点击“查看诊断”，窗口会直接显示服务错误；可一键复制反馈，无需寻找日志文件。";
    }
    if (log.find("thirdPartyAllowed=0") != log.npos
        && log.find("reject: raw transport enable") != log.npos
        && log.find("M3 motor output:") == log.npos) {
        return L"手柄未允许第三方接管。点击下方“打开飞智设置”，在飞智空间站设置中"
            L"开启“允许第三方应用接管映射”，再回这里启动增强。若设置被锁定，请先正常退出 Steam。";
    }
    if (log.find("ERROR: Vader acquisition renewal") != log.npos) {
        return L"未能维持手柄接管，增强已停止。请确认手柄连接正常，再启动增强；反复失败可复制诊断信息反馈。";
    }
    if (log.find("ordinary input source") != log.npos
        && (log.find("disconnected;") != log.npos || log.find("sustained outage") != log.npos)) {
        return L"运行中连续两秒没有收到手柄输入，增强已停止。请确认手柄连接正常再重试；这不代表启动时未接管成功。";
    }
    if (log.find("ERROR: VIIPER feedback stream failed") != log.npos) {
        return L"虚拟手柄反馈连接中断。请重新启动增强；若仍失败，点击“查看诊断”复制错误信息。";
    }
    const auto error = log.rfind("ERROR:");
    if (error != log.npos) {
        const auto end = log.find_first_of("\r\n", error);
        const auto line = log.substr(error, end == log.npos ? log.npos : end - error);
        return L"增强未能正常运行：" + Wide(line) + L"\r\n点击“查看诊断”可直接查看并复制详情。";
    }
    return L"增强停止或未完成启动，尚未收到明确原因。请点击“查看诊断”复制当前状态反馈。";
}

std::wstring DiagnosticDetails(std::string_view bridge, std::string_view service,
    std::string_view recovery)
{
    const auto summary = bridge.empty() ? std::wstring(L"还没有运行记录。请先在主窗口启动增强。")
        : (BridgeStartupReady(bridge) ? std::wstring(L"已有启动检查通过的记录，未发现明确错误；当前模式以主窗口为准。")
                                    : ExplainBridgeFailure(bridge, service));
    return L"【原因与建议】\r\n" + summary
        + L"\r\n\r\n【虚拟手柄服务】\r\n" + RelevantLines(service)
        + L"\r\n【接管程序】\r\n" + RelevantLines(bridge)
        + L"\r\n【最近一次恢复记录】\r\n" + RelevantLines(recovery);
}
}
