#include <windows.h>
#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <tlhelp32.h>
#include "settings.h"

#include <atomic>
#include <filesystem>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kWindowClass[] = L"VaderHapticsLauncherWindow";
constexpr wchar_t kBridgeEvent[] = L"Local\\VaderHapticsBridgeGracefulStop";
constexpr UINT kStatusMessage = WM_APP + 1;
constexpr int kStartButton = 1001;
constexpr int kNativeButton = 1002;
constexpr int kSettingsButton = 1003;
constexpr int kLogButton = 1004;
constexpr int kPathButton = 1005;
constexpr UINT_PTR kButtonTimer = 1;

HWND g_window = nullptr;
HWND g_statusLabel = nullptr;
HWND g_detailLabel = nullptr;
HWND g_pathLabel = nullptr;
std::atomic<bool> g_busy = false;
std::atomic<bool> g_expectedBridgeStop = false;
HFONT g_titleFont = nullptr;
HFONT g_bodyFont = nullptr;
HFONT g_smallFont = nullptr;

std::filesystem::path ExecutableDirectory()
{
    std::vector<wchar_t> path(32768);
    const DWORD size = GetModuleFileNameW(
        nullptr, path.data(), static_cast<DWORD>(path.size()));
    return std::filesystem::path(std::wstring(path.data(), size)).parent_path();
}

std::wstring Quote(const std::filesystem::path& value)
{
    return L"\"" + value.wstring() + L"\"";
}

std::filesystem::path BridgePath()
{
    return ExecutableDirectory() / L"vader_ds5_bridge.exe";
}

std::filesystem::path ConfigPath()
{
    return ExecutableDirectory() / L"config" / L"m3_calibration.json";
}

std::filesystem::path LogDirectory()
{
    return ExecutableDirectory() / L"logs";
}

std::filesystem::path SettingsPath()
{
    return ExecutableDirectory() / L"VaderHapticsLauncher.ini";
}

std::filesystem::path SpaceStationDirectory()
{
    return launcher::ReadSpaceStationDirectory(SettingsPath());
}

std::filesystem::path FlydigiOnboardDirectory()
{
    return SpaceStationDirectory() / L"Configs" / L"Controller" /
        L"f5" / L"onboard";
}

void RemoveProfileSnapshot(const std::filesystem::path& directory)
{
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

bool SnapshotFlydigiProfiles(std::filesystem::path& destination)
{
    destination = ExecutableDirectory() / L"cache" /
        (L"onboard-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()));
    std::error_code error;
    std::filesystem::create_directories(destination, error);
    if (error) return false;

    bool copied = false;
    bool failed = false;
    const auto source = FlydigiOnboardDirectory();
    WIN32_FIND_DATAW found = {};
    HANDLE search = FindFirstFileW((source / L"*.dat").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            if ((found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
                if (CopyFileW((source / found.cFileName).c_str(),
                        (destination / found.cFileName).c_str(), FALSE)) {
                    copied = true;
                }
                else {
                    failed = true;
                    break;
                }
            }
        } while (FindNextFileW(search, &found));
        if (GetLastError() != ERROR_NO_MORE_FILES && !failed) {
            failed = true;
        }
        FindClose(search);
    }
    else {
        failed = true;
    }
    if (!copied || failed) {
        RemoveProfileSnapshot(destination);
        destination.clear();
        return false;
    }
    return true;
}

bool BridgeActive()
{
    HANDLE event = OpenEventW(SYNCHRONIZE, FALSE, kBridgeEvent);
    if (!event) return false;
    CloseHandle(event);
    return true;
}

void PostStatus(std::wstring title, std::wstring detail)
{
    auto* payload = new std::pair<std::wstring, std::wstring>(
        std::move(title), std::move(detail));
    PostMessageW(g_window, kStatusMessage, 0,
        reinterpret_cast<LPARAM>(payload));
}

bool StartProcess(const std::filesystem::path& exe,
    const std::wstring& arguments, bool hidden, HANDLE* processHandle,
    const std::filesystem::path& logPath = {})
{
    std::wstring command = Quote(exe);
    if (!arguments.empty()) command += L" " + arguments;

    SECURITY_ATTRIBUTES security = {};
    security.nLength = sizeof(security);
    security.bInheritHandle = TRUE;
    HANDLE log = INVALID_HANDLE_VALUE;
    if (!logPath.empty()) {
        std::error_code error;
        std::filesystem::create_directories(logPath.parent_path(), error);
        log = CreateFileW(logPath.c_str(), GENERIC_WRITE,
            FILE_SHARE_READ | FILE_SHARE_WRITE, &security, CREATE_ALWAYS,
            FILE_ATTRIBUTE_NORMAL, nullptr);
        if (log == INVALID_HANDLE_VALUE) return false;
    }

    STARTUPINFOW startup = {};
    startup.cb = sizeof(startup);
    if (log != INVALID_HANDLE_VALUE) {
        startup.dwFlags |= STARTF_USESTDHANDLES;
        startup.hStdOutput = log;
        startup.hStdError = log;
        startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    }
    if (hidden) {
        startup.dwFlags |= STARTF_USESHOWWINDOW;
        startup.wShowWindow = SW_HIDE;
    }

    PROCESS_INFORMATION process = {};
    const DWORD flags = hidden ? CREATE_NO_WINDOW : 0;
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr,
        nullptr, log != INVALID_HANDLE_VALUE, flags, nullptr,
        exe.parent_path().c_str(), &startup, &process);
    if (log != INVALID_HANDLE_VALUE) CloseHandle(log);
    if (!started) return false;
    CloseHandle(process.hThread);
    if (processHandle) *processHandle = process.hProcess;
    else CloseHandle(process.hProcess);
    return true;
}

bool RunBridgeCommand(const std::wstring& arguments,
    const std::filesystem::path& logPath, DWORD timeoutMs)
{
    HANDLE process = nullptr;
    if (!StartProcess(BridgePath(), arguments, true, &process, logPath)) {
        return false;
    }
    const DWORD wait = WaitForSingleObject(process, timeoutMs);
    DWORD exitCode = ERROR_GEN_FAILURE;
    if (wait == WAIT_OBJECT_0) GetExitCodeProcess(process, &exitCode);
    CloseHandle(process);
    return wait == WAIT_OBJECT_0 && exitCode == 0;
}

std::vector<DWORD> ProcessIds(const wchar_t* name)
{
    std::vector<DWORD> result;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return result;
    PROCESSENTRY32W entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, name) == 0) {
                result.push_back(entry.th32ProcessID);
            }
        } while (Process32NextW(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return result;
}

std::filesystem::path FirstProcessPath(const wchar_t* name)
{
    const auto ids = ProcessIds(name);
    if (ids.empty()) return {};
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION,
        FALSE, ids.front());
    if (!process) return {};
    std::vector<wchar_t> path(32768);
    DWORD size = static_cast<DWORD>(path.size());
    const BOOL queried = QueryFullProcessImageNameW(
        process, 0, path.data(), &size);
    CloseHandle(process);
    return queried ? std::filesystem::path(std::wstring(path.data(), size))
                   : std::filesystem::path{};
}

void RefreshPathLabel()
{
    const auto directory = SpaceStationDirectory();
    const auto label = directory.empty() ? L"空间站：未配置，请选择安装目录"
        : L"空间站：" + directory.wstring();
    SetWindowTextW(g_pathLabel, label.c_str());
}

bool DetectSpaceStation()
{
    if (!SpaceStationDirectory().empty()) return true;
    for (const auto* name : {L"Flydigi Space Station.exe", L"SpaceStationService.exe"}) {
        const auto directory = FirstProcessPath(name).parent_path();
        if (launcher::IsSpaceStationDirectory(directory)
            && launcher::SaveSpaceStationDirectory(SettingsPath(), directory)) return true;
    }
    return false;
}

bool SelectSpaceStation()
{
    IFileOpenDialog* dialog = nullptr;
    HRESULT result = CoCreateInstance(CLSID_FileOpenDialog, nullptr,
        CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));
    if (FAILED(result)) {
        PostStatus(L"无法打开目录选择", L"请重新打开启动器后再试。");
        return false;
    }
    DWORD options = 0;
    result = dialog->GetOptions(&options);
    if (SUCCEEDED(result)) result = dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
    dialog->SetTitle(L"选择飞智空间站安装目录（包含 SpaceStationService.exe）");
    if (SUCCEEDED(result)) result = dialog->Show(g_window);
    std::filesystem::path directory;
    IShellItem* item = nullptr;
    if (SUCCEEDED(result)) result = dialog->GetResult(&item);
    if (SUCCEEDED(result)) {
        PWSTR path = nullptr;
        result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
        if (SUCCEEDED(result)) directory = path;
        CoTaskMemFree(path);
        item->Release();
    }
    dialog->Release();
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) return false;
    if (FAILED(result) || !launcher::IsSpaceStationDirectory(directory)) {
        PostStatus(L"请选择空间站安装目录",
            L"目录中需要同时包含空间站程序和 SpaceStationService.exe。");
        return false;
    }
    if (!launcher::SaveSpaceStationDirectory(SettingsPath(), directory)) {
        PostStatus(L"路径未保存", L"无法写入启动器配置，请检查安装目录的写入权限。");
        return false;
    }
    RefreshPathLabel();
    PostStatus(L"空间站路径已保存", L"现在可以启动 PS5 增强；下次无需重新选择。");
    return true;
}

bool EnsureSpaceStation()
{
    if (DetectSpaceStation()) {
        RefreshPathLabel();
        return true;
    }
    return SelectSpaceStation();
}

bool CheckEnhancedFiles()
{
    wchar_t localAppData[32768] = {};
    GetEnvironmentVariableW(L"LOCALAPPDATA", localAppData,
        static_cast<DWORD>(std::size(localAppData)));
    const std::pair<std::filesystem::path, const wchar_t*> required[] = {
        {BridgePath(), L"缺少桥接程序，请重新解压完整程序包。"},
        {ConfigPath(), L"缺少震动配置，请重新解压完整程序包。"},
        {std::filesystem::path(localAppData) / L"VIIPER" / L"viiper.exe",
            L"未找到 VIIPER，请按安装说明安装 v0.0.9 兼容运行时。"},
        {L"C:\\Program Files\\Nefarius Software Solutions\\HidHide\\x64\\HidHideCLI.exe",
            L"未找到 HidHide，请按安装说明安装后重试。"}
    };
    for (const auto& [path, detail] : required) {
        std::error_code error;
        if (!std::filesystem::is_regular_file(path, error)) {
            PostStatus(L"启动前检查未通过", detail);
            return false;
        }
    }
    return true;
}

void ConfigureSpaceStation()
{
    if (g_busy.exchange(true)) return;
    if (!BridgeActive()) SelectSpaceStation();
    g_busy = false;
}

bool ShutdownSteam()
{
    const auto steam = FirstProcessPath(L"steam.exe");
    if (steam.empty()) return true;
    HANDLE command = nullptr;
    if (!StartProcess(steam, L"-shutdown", true, &command)) return false;
    WaitForSingleObject(command, 10000);
    CloseHandle(command);
    for (int i = 0; i < 100; ++i) {
        if (ProcessIds(L"steam.exe").empty()) return true;
        Sleep(100);
    }
    return false;
}

BOOL CALLBACK CloseWindowForProcess(HWND window, LPARAM parameter)
{
    DWORD owner = 0;
    GetWindowThreadProcessId(window, &owner);
    if (owner == static_cast<DWORD>(parameter) && IsWindowVisible(window)) {
        PostMessageW(window, WM_CLOSE, 0, 0);
    }
    return TRUE;
}

void StopNamedProcess(const wchar_t* name)
{
    const auto ids = ProcessIds(name);
    for (DWORD id : ids) {
        EnumWindows(CloseWindowForProcess, static_cast<LPARAM>(id));
    }
    if (!ids.empty()) Sleep(1200);
    for (DWORD id : ids) {
        HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE,
            FALSE, id);
        if (!process) continue;
        if (WaitForSingleObject(process, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process, 0);
        }
        CloseHandle(process);
    }
}

void StopSpaceStation()
{
    StopNamedProcess(L"Flydigi Space Station.exe");
    StopNamedProcess(L"SpaceStationService.exe");
}

bool StartSpaceStation(bool showUi)
{
    const auto directory = SpaceStationDirectory();
    const auto service = directory / L"SpaceStationService.exe";
    const auto app = directory / L"Flydigi Space Station.exe";
    if (!std::filesystem::exists(service) || !std::filesystem::exists(app)) {
        return false;
    }
    if (ProcessIds(L"SpaceStationService.exe").empty()) {
        StartProcess(service, L"", true, nullptr);
        Sleep(800);
    }
    if (showUi && ProcessIds(L"Flydigi Space Station.exe").empty()) {
        return StartProcess(app, L"", false, nullptr);
    }
    return true;
}

bool ReturnNative(bool showSpaceStation)
{
    StopSpaceStation();
    const bool native = RunBridgeCommand(L"mode native",
        LogDirectory() / L"native.log", 45000);
    if (native) StartSpaceStation(showSpaceStation);
    return native;
}

void WatchBridge(HANDLE process,
    std::filesystem::path profileSnapshot)
{
    std::thread([process, profileSnapshot = std::move(profileSnapshot)] {
        WaitForSingleObject(process, INFINITE);
        CloseHandle(process);
        RemoveProfileSnapshot(profileSnapshot);
        if (g_expectedBridgeStop.exchange(false)) return;
        while (g_busy.load()) Sleep(50);
        bool idle = false;
        if (!g_busy.compare_exchange_strong(idle, true)) return;
        const bool restored = ReturnNative(false);
        PostStatus(restored ? L"Native 已自动恢复" : L"自动恢复失败",
            restored
                ? L"增强进程已停止；飞智空间站已重新启动。"
                : L"增强进程异常停止，请打开 logs\\native.log。");
        g_busy = false;
    }).detach();
}

void StartEnhanced()
{
    if (g_busy.exchange(true)) return;
    if (BridgeActive() || !CheckEnhancedFiles() || !EnsureSpaceStation()) {
        g_busy = false;
        return;
    }
    const bool steamRunning = !ProcessIds(L"steam.exe").empty();
    if (steamRunning) {
        const int answer = MessageBoxW(g_window,
            L"Steam 正在后台接管飞智映射，PS5 增强可能无法启动。\r\n\r\n"
            L"是否先正常退出 Steam？桥接成功后可以重新打开 Steam。",
            L"发现映射占用", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer != IDYES) {
            g_busy = false;
            return;
        }
    }
    std::thread([steamRunning] {
        PostStatus(L"正在准备 PS5 增强", L"关闭飞智空间站并清理上一次会话…");
        if (steamRunning && !ShutdownSteam()) {
            PostStatus(L"Steam 仍在占用", L"请先手动退出 Steam，再重新启动增强模式。");
            g_busy = false;
            return;
        }
        std::filesystem::path profileSnapshot;
        if (!SnapshotFlydigiProfiles(profileSnapshot)) {
            PostStatus(L"启动失败",
                L"无法只读快照飞智板载配置；空间站保持 Native。");
            g_busy = false;
            return;
        }
        StopSpaceStation();
        if (!RunBridgeCommand(L"mode native", LogDirectory() / L"native.log",
                45000)) {
            PostStatus(L"启动失败", L"Native 清理未通过；未进入增强模式。");
            StartSpaceStation(false);
            RemoveProfileSnapshot(profileSnapshot);
            g_busy = false;
            return;
        }

        const std::wstring arguments = L"mode ps5 --motor-output on "
            L"--input-source mi01 --nodelay --config " + Quote(ConfigPath()) +
            L" --flydigi-onboard-dir " + Quote(profileSnapshot);
        HANDLE process = nullptr;
        g_expectedBridgeStop = false;
        if (!StartProcess(BridgePath(), arguments, true, &process,
                LogDirectory() / L"bridge.log")) {
            PostStatus(L"启动失败", L"无法启动桥接程序；请检查安装目录。");
            StartSpaceStation(false);
            RemoveProfileSnapshot(profileSnapshot);
            g_busy = false;
            return;
        }

        bool active = false;
        for (int i = 0; i < 120; ++i) {
            if (BridgeActive()) { active = true; break; }
            if (WaitForSingleObject(process, 0) == WAIT_OBJECT_0) break;
            Sleep(100);
        }
        if (active) {
            PostStatus(L"PS5 增强已接管",
                L"虚拟 DualSense 正在运行 · 板载手柄键映射已启用");
            WatchBridge(process, std::move(profileSnapshot));
        }
        else {
            CloseHandle(process);
            ReturnNative(false);
            RemoveProfileSnapshot(profileSnapshot);
            PostStatus(L"启动失败", L"桥接程序未通过启动门；已自动恢复 Native。");
        }
        g_busy = false;
    }).detach();
}

void OpenFlydigiSettings()
{
    if (g_busy.exchange(true)) return;
    if (!EnsureSpaceStation()) {
        g_busy = false;
        return;
    }
    const bool steamRunning = !ProcessIds(L"steam.exe").empty();
    if (steamRunning) {
        const int answer = MessageBoxW(g_window,
            L"Steam 正在后台接管映射，所以飞智空间站仍会锁定设置。\r\n\r\n"
            L"是否正常退出 Steam 并打开飞智设置？",
            L"发现映射占用", MB_YESNOCANCEL | MB_ICONQUESTION);
        if (answer != IDYES) {
            g_busy = false;
            return;
        }
    }
    std::thread([steamRunning] {
        PostStatus(L"正在打开飞智设置", L"释放 VaderHaptics 与其他第三方映射占用…");
        if (steamRunning && !ShutdownSteam()) {
            PostStatus(L"Steam 仍在占用", L"请手动退出 Steam 后再试。");
            g_busy = false;
            return;
        }
        if (BridgeActive()) g_expectedBridgeStop = true;
        const bool ok = ReturnNative(true);
        PostStatus(ok ? L"飞智设置已就绪" : L"打开设置失败",
            ok ? L"空间站已打开；保存的配置会留在飞智板载配置中。"
               : L"Native 恢复门未通过，请查看 native.log。");
        g_busy = false;
    }).detach();
}

void SwitchToNative(bool showSpaceStation)
{
    if (g_busy.exchange(true)) return;
    if (BridgeActive()) g_expectedBridgeStop = true;
    std::thread([showSpaceStation] {
        PostStatus(L"正在返回 Native", L"归零振动、释放接管并恢复飞智空间站…");
        const bool ok = ReturnNative(showSpaceStation);
        const bool steamOccupies = !ProcessIds(L"steam.exe").empty();
        PostStatus(ok ? (steamOccupies ? L"Native · Steam 正在接管" : L"Native 已恢复")
                      : L"恢复失败",
            ok ? (steamOccupies
                    ? L"VaderHaptics 已释放；飞智设置仍会被后台 Steam 锁定。"
                    : L"手柄已交还飞智空间站，可以修改板载配置。")
               : L"恢复门未通过；请打开 logs\\native.log 查看原因。");
        g_busy = false;
    }).detach();
}

void OpenLogs()
{
    std::error_code error;
    std::filesystem::create_directories(LogDirectory(), error);
    ShellExecuteW(g_window, L"open", LogDirectory().c_str(), nullptr,
        nullptr, SW_SHOWNORMAL);
}

void SetControlFont(HWND control, HFONT font)
{
    SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
}

HWND MakeButton(HWND parent, int id, const wchar_t* text,
    int x, int y, int width, int height)
{
    HWND button = CreateWindowW(L"BUTTON", text,
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW, x, y, width, height,
        parent, reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)),
        GetModuleHandleW(nullptr), nullptr);
    SetControlFont(button, g_bodyFont);
    return button;
}

void DrawButton(const DRAWITEMSTRUCT& item)
{
    const bool primary = item.CtlID == kStartButton;
    const bool pressed = (item.itemState & ODS_SELECTED) != 0;
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    COLORREF fill = primary ? RGB(16, 103, 186) : RGB(28, 34, 44);
    if (pressed) fill = primary ? RGB(10, 78, 145) : RGB(20, 25, 33);
    if (disabled) fill = RGB(31, 34, 40);
    HBRUSH brush = CreateSolidBrush(fill);
    FillRect(item.hDC, &item.rcItem, brush);
    DeleteObject(brush);
    HPEN pen = CreatePen(PS_SOLID, 1,
        primary ? RGB(52, 152, 255) : RGB(66, 76, 92));
    HGDIOBJ oldPen = SelectObject(item.hDC, pen);
    HGDIOBJ oldBrush = SelectObject(item.hDC, GetStockObject(NULL_BRUSH));
    RoundRect(item.hDC, item.rcItem.left, item.rcItem.top,
        item.rcItem.right, item.rcItem.bottom, 12, 12);
    SelectObject(item.hDC, oldBrush);
    SelectObject(item.hDC, oldPen);
    DeleteObject(pen);
    wchar_t text[128] = {};
    GetWindowTextW(item.hwndItem, text, static_cast<int>(std::size(text)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(112, 118, 128) : RGB(240, 245, 252));
    SelectObject(item.hDC, g_bodyFont);
    RECT rect = item.rcItem;
    DrawTextW(item.hDC, text, -1, &rect,
        DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if ((item.itemState & ODS_FOCUS) != 0) {
        InflateRect(&rect, -4, -4);
        DrawFocusRect(item.hDC, &rect);
    }
}

LRESULT CALLBACK WindowProcedure(HWND window, UINT message,
    WPARAM wParam, LPARAM lParam)
{
    switch (message) {
    case WM_CREATE: {
        g_titleFont = CreateFontW(-30, 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI Variable Display");
        g_bodyFont = CreateFontW(-17, 0, 0, 0, FW_MEDIUM, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        g_smallFont = CreateFontW(-14, 0, 0, 0, FW_NORMAL, FALSE, FALSE,
            FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
        HWND title = CreateWindowW(L"STATIC", L"VADER HAPTICS",
            WS_CHILD | WS_VISIBLE, 36, 26, 430, 42, window, nullptr,
            nullptr, nullptr);
        SetControlFont(title, g_titleFont);
        const bool bridgeActive = BridgeActive();
        const bool steamOccupies = !ProcessIds(L"steam.exe").empty();
        g_statusLabel = CreateWindowW(L"STATIC",
            bridgeActive ? L"PS5 增强已接管"
                         : (steamOccupies ? L"Native · Steam 正在接管"
                                          : L"Native · 等待启动"),
            WS_CHILD | WS_VISIBLE, 52, 105, 420, 30, window, nullptr,
            nullptr, nullptr);
        SetControlFont(g_statusLabel, g_bodyFont);
        g_detailLabel = CreateWindowW(L"STATIC",
            bridgeActive ? L"虚拟 DualSense 正在运行 · 飞智设置暂时锁定"
                         : (steamOccupies
                            ? L"VaderHaptics 已释放 · 飞智设置仍被后台 Steam 锁定"
                            : L"原生手柄输入可用 · 飞智空间站可以设置"),
            WS_CHILD | WS_VISIBLE, 52, 139, 442, 48, window, nullptr,
            nullptr, nullptr);
        SetControlFont(g_detailLabel, g_smallFont);
        MakeButton(window, kStartButton, L"启动 PS5 增强", 36, 205, 250, 54);
        MakeButton(window, kNativeButton, L"返回 Native", 300, 205, 210, 54);
        MakeButton(window, kSettingsButton, L"打开飞智设置", 36, 277, 250, 46);
        MakeButton(window, kLogButton, L"打开运行日志", 300, 277, 210, 46);
        g_pathLabel = CreateWindowW(L"STATIC", L"",
            WS_CHILD | WS_VISIBLE | SS_PATHELLIPSIS, 42, 346, 465, 24,
            window, nullptr, nullptr, nullptr);
        SetControlFont(g_pathLabel, g_smallFont);
        DetectSpaceStation();
        RefreshPathLabel();
        MakeButton(window, kPathButton, L"选择空间站目录…", 36, 378, 474, 38);
        HWND note = CreateWindowW(L"STATIC",
            L"增强模式会临时关闭空间站；返回 Native 后会完整释放接管。\r\n"
            L"拓展键若映射为手柄键会转成 DS5；键盘、宏和连发保持 Native 专属。",
            WS_CHILD | WS_VISIBLE, 42, 438, 465, 52, window, nullptr,
            nullptr, nullptr);
        SetControlFont(note, g_smallFont);
        SetTimer(window, kButtonTimer, 150, nullptr);
        return 0;
    }
    case WM_CTLCOLORSTATIC: {
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(220, 229, 240));
        return reinterpret_cast<LRESULT>(GetStockObject(HOLLOW_BRUSH));
    }
    case WM_DRAWITEM:
        DrawButton(*reinterpret_cast<DRAWITEMSTRUCT*>(lParam));
        return TRUE;
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kStartButton: StartEnhanced(); break;
        case kNativeButton: SwitchToNative(false); break;
        case kSettingsButton: OpenFlydigiSettings(); break;
        case kLogButton: OpenLogs(); break;
        case kPathButton: ConfigureSpaceStation(); break;
        }
        return 0;
    case WM_TIMER:
        if (wParam == kButtonTimer) {
            const bool busy = g_busy.load();
            const bool active = BridgeActive();
            static int previousState = -1;
            const int state = (busy ? 1 : 0) | (active ? 2 : 0);
            if (state == previousState) return 0;
            previousState = state;
            EnableWindow(GetDlgItem(window, kStartButton), !busy && !active);
            EnableWindow(GetDlgItem(window, kNativeButton), !busy);
            EnableWindow(GetDlgItem(window, kSettingsButton), !busy);
            EnableWindow(GetDlgItem(window, kPathButton), !busy && !active);
            SetWindowTextW(GetDlgItem(window, kStartButton),
                busy ? L"正在处理，请稍候…" : (active ? L"PS5 增强运行中" : L"启动 PS5 增强"));
        }
        return 0;
    case kStatusMessage: {
        auto* payload = reinterpret_cast<std::pair<std::wstring, std::wstring>*>(lParam);
        SetWindowTextW(g_statusLabel, payload->first.c_str());
        SetWindowTextW(g_detailLabel, payload->second.c_str());
        delete payload;
        InvalidateRect(window, nullptr, FALSE);
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT paint = {};
        HDC dc = BeginPaint(window, &paint);
        RECT client = {};
        GetClientRect(window, &client);
        TRIVERTEX vertices[2] = {
            {0, 0, 0x0700, 0x0B00, 0x1200, 0},
            {client.right, client.bottom, 0x1100, 0x1820, 0x2500, 0}};
        GRADIENT_RECT gradient = {0, 1};
        GradientFill(dc, vertices, 2, &gradient, 1, GRADIENT_FILL_RECT_V);
        RECT card = {36, 92, 510, 190};
        HBRUSH cardBrush = CreateSolidBrush(RGB(18, 24, 34));
        FillRect(dc, &card, cardBrush);
        DeleteObject(cardBrush);
        HPEN accent = CreatePen(PS_SOLID, 3, RGB(31, 143, 255));
        HGDIOBJ old = SelectObject(dc, accent);
        MoveToEx(dc, 36, 92, nullptr);
        LineTo(dc, 36, 190);
        SelectObject(dc, old);
        DeleteObject(accent);
        EndPaint(window, &paint);
        return 0;
    }
    case WM_CLOSE:
        if (g_busy) {
            MessageBoxW(window, L"当前操作尚未完成，请稍候。",
                L"VaderHaptics", MB_OK | MB_ICONINFORMATION);
            return 0;
        }
        if (BridgeActive()) {
            const int answer = MessageBoxW(window,
                L"PS5 增强仍在运行。退出前返回 Native 吗？",
                L"VaderHaptics", MB_YESNOCANCEL | MB_ICONQUESTION);
            if (answer == IDCANCEL) return 0;
            if (answer == IDYES) g_expectedBridgeStop = true;
            if (answer == IDYES && !ReturnNative(false)) {
                MessageBoxW(window, L"Native 恢复失败，启动器不会静默退出。",
                    L"VaderHaptics", MB_OK | MB_ICONERROR);
                return 0;
            }
        }
        DestroyWindow(window);
        return 0;
    case WM_DESTROY:
        KillTimer(window, kButtonTimer);
        DeleteObject(g_titleFont);
        DeleteObject(g_bodyFont);
        DeleteObject(g_smallFont);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    if (FAILED(com)) return 1;
    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.hInstance = instance;
    windowClass.lpfnWndProc = WindowProcedure;
    windowClass.lpszClassName = kWindowClass;
    windowClass.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    windowClass.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
    windowClass.hIcon = LoadIconW(nullptr, IDI_APPLICATION);
    windowClass.hIconSm = windowClass.hIcon;
    if (!RegisterClassExW(&windowClass)) return 1;
    g_window = CreateWindowExW(0, kWindowClass,
        L"VaderHaptics · Controller Runtime", WS_OVERLAPPED | WS_CAPTION |
        WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
        566, 550, nullptr, nullptr, instance, nullptr);
    if (!g_window) return 1;
    const BOOL dark = TRUE;
    DwmSetWindowAttribute(g_window, 20, &dark, sizeof(dark));
    ShowWindow(g_window, showCommand);
    UpdateWindow(g_window);
    MSG message = {};
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(g_window, &message)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
    }
    CoUninitialize();
    return static_cast<int>(message.wParam);
}
