#include "window_enumerator.h"

#include <dwmapi.h>
#include <shellapi.h>

#include <algorithm>
#include <filesystem>

namespace ts3stream::media {
namespace {

bool isCloaked(HWND hwnd) {
    DWORD cloaked = 0;
    const HRESULT hr = DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked));
    return SUCCEEDED(hr) && cloaked != 0;
}

struct ProcessInfo {
    std::wstring name;
    std::wstring path;
};

ProcessInfo processInfoForPid(DWORD pid) {
    ProcessInfo result;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) {
        return result;
    }

    wchar_t path[4096] = {};
    DWORD size = static_cast<DWORD>(std::size(path));
    if (QueryFullProcessImageNameW(process, 0, path, &size)) {
        result.path = path;
        result.name = std::filesystem::path(path).filename().wstring();
    }
    CloseHandle(process);
    return result;
}

HICON iconForWindow(HWND hwnd) {
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_BIG, 0, SMTO_ABORTIFHUNG, 100, &result) && result) {
        return reinterpret_cast<HICON>(result);
    }
    if (SendMessageTimeoutW(hwnd, WM_GETICON, ICON_SMALL, 0, SMTO_ABORTIFHUNG, 100, &result) && result) {
        return reinterpret_cast<HICON>(result);
    }
    const LONG_PTR classIcon = GetClassLongPtrW(hwnd, GCLP_HICON);
    if (classIcon) {
        return reinterpret_cast<HICON>(classIcon);
    }
    return LoadIconW(nullptr, IDI_APPLICATION);
}

BOOL CALLBACK windowCallback(HWND hwnd, LPARAM data) {
    auto* windows = reinterpret_cast<std::vector<WindowInfo>*>(data);
    if (!windows || !IsWindowVisible(hwnd) || isCloaked(hwnd)) {
        return TRUE;
    }

    const LONG_PTR exStyle = GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
    if ((exStyle & WS_EX_TOOLWINDOW) != 0) {
        return TRUE;
    }

    const int titleLength = GetWindowTextLengthW(hwnd);
    if (titleLength <= 0) {
        return TRUE;
    }

    std::wstring title(static_cast<size_t>(titleLength) + 1, L'\0');
    GetWindowTextW(hwnd, title.data(), titleLength + 1);
    title.resize(static_cast<size_t>(titleLength));
    if (title.empty()) {
        return TRUE;
    }

    DWORD pid = 0;
    GetWindowThreadProcessId(hwnd, &pid);
    if (pid == GetCurrentProcessId()) {
        return TRUE;
    }

    ProcessInfo processInfo = processInfoForPid(pid);
    if (processInfo.name == L"ts3client_win64.exe") {
        return TRUE;
    }

    WindowInfo info;
    info.hwnd = hwnd;
    info.title = std::move(title);
    info.processName = processInfo.name;
    info.processPath = processInfo.path;
    info.processId = pid;
    info.icon = iconForWindow(hwnd);
    windows->push_back(std::move(info));
    return TRUE;
}

BOOL CALLBACK monitorCallback(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* monitors = reinterpret_cast<std::vector<MonitorInfo>*>(data);
    if (!monitors || !monitor) {
        return TRUE;
    }

    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, &info)) {
        return TRUE;
    }

    MonitorInfo monitorInfo;
    monitorInfo.handle = monitor;
    monitorInfo.name = info.szDevice;
    monitorInfo.rect = info.rcMonitor;
    monitorInfo.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    monitors->push_back(std::move(monitorInfo));
    return TRUE;
}

} // namespace

std::vector<WindowInfo> enumerateWindows() {
    std::vector<WindowInfo> windows;
    EnumWindows(windowCallback, reinterpret_cast<LPARAM>(&windows));

    std::sort(windows.begin(), windows.end(), [](const WindowInfo& a, const WindowInfo& b) {
        if (a.processName != b.processName) {
            return a.processName < b.processName;
        }
        return a.title < b.title;
    });
    return windows;
}

std::vector<MonitorInfo> enumerateMonitors() {
    std::vector<MonitorInfo> monitors;
    EnumDisplayMonitors(nullptr, nullptr, monitorCallback, reinterpret_cast<LPARAM>(&monitors));
    std::sort(monitors.begin(), monitors.end(), [](const MonitorInfo& a, const MonitorInfo& b) {
        if (a.primary != b.primary) {
            return a.primary > b.primary;
        }
        return a.name < b.name;
    });
    return monitors;
}

} // namespace ts3stream::media
