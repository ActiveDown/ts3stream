#pragma once

#include <windows.h>

#include <cstdint>
#include <string>
#include <vector>

namespace ts3stream::media {

struct WindowInfo {
    HWND hwnd = nullptr;
    std::wstring title;
    std::wstring processName;
    std::wstring processPath;
    DWORD processId = 0;
    HICON icon = nullptr;
};

struct MonitorInfo {
    HMONITOR handle = nullptr;
    std::wstring name;
    RECT rect{};
    bool primary = false;
};

std::vector<WindowInfo> enumerateWindows();
std::vector<MonitorInfo> enumerateMonitors();

} // namespace ts3stream::media
