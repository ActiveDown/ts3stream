#include "log.h"

#include <windows.h>

#include <cstdarg>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

namespace ts3stream::diag {
namespace {

std::mutex g_logMutex;
std::ofstream g_logStream;
std::wstring g_dataDir;
bool g_initialized = false;

std::wstring makeDataDir() {
    wchar_t buffer[MAX_PATH * 2] = {};
    DWORD len = GetEnvironmentVariableW(L"APPDATA", buffer, static_cast<DWORD>(std::size(buffer)));
    std::wstring base;
    if (len > 0 && len < std::size(buffer)) {
        base.assign(buffer, len);
    } else {
        base = L".";
    }
    return base + L"\\TS3Client\\plugins\\ts3stream";
}

std::string timestamp() {
    SYSTEMTIME st{};
    GetLocalTime(&st);
    char out[64] = {};
    std::snprintf(out, sizeof(out), "%04u-%02u-%02u %02u:%02u:%02u.%03u",
                  st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    return out;
}

} // namespace

std::wstring dataDir() {
    if (g_dataDir.empty()) {
        g_dataDir = makeDataDir();
    }
    return g_dataDir;
}

std::wstring moduleDir() {
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCWSTR>(&moduleDir), &module)) {
        return {};
    }
    wchar_t path[MAX_PATH * 2] = {};
    const DWORD length = GetModuleFileNameW(module, path, static_cast<DWORD>(std::size(path)));
    if (length == 0 || length >= std::size(path)) {
        return {};
    }
    std::filesystem::path file(path);
    return file.parent_path().wstring();
}

std::string toUtf8(const std::wstring& value) {
    if (value.empty()) {
        return {};
    }
    int size = WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                                   nullptr, 0, nullptr, nullptr);
    if (size <= 0) {
        return {};
    }
    std::string out(static_cast<size_t>(size), '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), static_cast<int>(value.size()),
                        out.data(), size, nullptr, nullptr);
    return out;
}

void initLog() {
    std::lock_guard<std::mutex> lock(g_logMutex);
    if (g_initialized) {
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(dataDir(), ec);
    const auto path = std::filesystem::path(dataDir()) / L"diag.log";
    g_logStream.open(path, std::ios::out | std::ios::app | std::ios::binary);
    g_initialized = true;
    g_logStream << "\n===== TS3Stream diagnostics " << timestamp() << " =====\n";
    g_logStream.flush();
}

void log(const char* fmt, ...) {
    if (!fmt) {
        return;
    }

    char message[4096] = {};
    va_list args;
    va_start(args, fmt);
    std::vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    const std::string line = "[" + timestamp() + "] " + message + "\n";

    {
        std::lock_guard<std::mutex> lock(g_logMutex);
        if (g_logStream.is_open()) {
            g_logStream << line;
            g_logStream.flush();
        }
    }

    OutputDebugStringA(line.c_str());
}

} // namespace ts3stream::diag
