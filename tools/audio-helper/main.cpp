#include "media/wasapi_capture.h"
#include "log.h"

#include <windows.h>
#include <objbase.h>
#include <tlhelp32.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <string>

namespace {

constexpr uint32_t kMagic = 0x41554453; // "AUDS"

struct AudioPacketHeader {
    uint32_t magic = kMagic;
    uint32_t frames = 0;
    uint32_t channels = 0;
    uint32_t sampleRate = 0;
    int32_t peak = 0;
    int32_t reserved = 0;
    int64_t timestampUs = 0;
};

std::atomic<bool> g_pipeBroken{false};

int countTs3Processes() {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return -1;
    }

    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    int count = 0;
    if (Process32FirstW(snapshot, &entry)) {
        do {
            if (_wcsicmp(entry.szExeFile, L"ts3client_win64.exe") == 0) {
                ++count;
            }
        } while (Process32NextW(snapshot, &entry));
    } else {
        count = -1;
    }
    CloseHandle(snapshot);
    return count;
}

bool isTs3ExecutablePath(const wchar_t* path) {
    if (path == nullptr) {
        return false;
    }
    const wchar_t* fileName = path;
    for (const wchar_t* p = path; *p != L'\0'; ++p) {
        if (*p == L'\\' || *p == L'/') {
            fileName = p + 1;
        }
    }
    return _wcsicmp(fileName, L"ts3client_win64.exe") == 0;
}

bool isTs3Process(DWORD pid) {
    if (pid == 0) {
        return true;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        // Unknown target: refuse APP capture rather than risk capturing TS3.
        ts3stream::diag::log("audio helper: cannot query target pid=%lu (error=%lu), refusing",
                             pid, GetLastError());
        return true;
    }

    std::wstring imagePath(32768, L'\0');
    DWORD size = static_cast<DWORD>(imagePath.size());
    bool isTs3 = true;
    if (QueryFullProcessImageNameW(process, 0, imagePath.data(), &size)) {
        imagePath.resize(size);
        isTs3 = isTs3ExecutablePath(imagePath.c_str());
    }
    CloseHandle(process);
    return isTs3;
}

bool isProcessAlive(DWORD pid) {
    if (pid == 0) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
        // ERROR_INVALID_PARAMETER means no process with this PID exists.
        // Access denied still means a process exists that we cannot query.
        return GetLastError() == ERROR_ACCESS_DENIED;
    }
    DWORD exitCode = 0;
    const bool alive =
        GetExitCodeProcess(process, &exitCode) != FALSE && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return alive;
}

bool writeAll(HANDLE pipe, const void* data, DWORD size) {
    const auto* bytes = static_cast<const uint8_t*>(data);
    DWORD written = 0;
    while (written < size) {
        DWORD chunk = 0;
        if (!WriteFile(pipe, bytes + written, size - written, &chunk, nullptr) || chunk == 0) {
            g_pipeBroken.store(true);
            return false;
        }
        written += chunk;
    }
    return true;
}

void writeErrorLine(HANDLE pipe, const char* line) {
    if (g_pipeBroken.load()) {
        return;
    }
    DWORD written = 0;
    WriteFile(pipe, line, static_cast<DWORD>(std::strlen(line)), &written, nullptr);
}

bool readLine(HANDLE pipe, std::string& line) {
    line.clear();
    char ch = 0;
    DWORD read = 0;
    while (ReadFile(pipe, &ch, 1, &read, nullptr) && read == 1) {
        if (ch == '\n' || ch == '\r') {
            return !line.empty();
        }
        line.push_back(ch);
        if (line.size() > 128) {
            return false;
        }
    }
    return false;
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    ts3stream::diag::initLog();
    ts3stream::diag::log("audio helper starting");
    if (argc < 2) {
        return 1;
    }

    const std::wstring pipeName = argv[1];
    HANDLE pipe = CreateNamedPipeW(
        pipeName.c_str(),
        PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
        1,
        64 * 1024,
        64 * 1024,
        0,
        nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        return 2;
    }

    const BOOL connected = ConnectNamedPipe(pipe, nullptr);
    if (!connected && GetLastError() != ERROR_PIPE_CONNECTED) {
        CloseHandle(pipe);
        return 3;
    }

    std::string command;
    if (!readLine(pipe, command)) {
        ts3stream::diag::log("audio helper command read failed");
        CloseHandle(pipe);
        return 4;
    }
    ts3stream::diag::log("audio helper command: %s", command.c_str());

    DWORD targetPid = 0;
    bool includeTree = false;
    if (std::sscanf(command.c_str(), "SYSTEM %lu", &targetPid) == 1) {
        includeTree = false;
    } else if (std::sscanf(command.c_str(), "APP %lu", &targetPid) == 1) {
        includeTree = true;
    } else {
        CloseHandle(pipe);
        return 5;
    }

    // APP mode captures the target process directly. Capturing a TS3 client
    // would feed its playback back into the stream, so refuse it outright.
    if (includeTree && isTs3Process(targetPid)) {
        ts3stream::diag::log("audio helper: refusing to capture TS3 process (pid=%lu)", targetPid);
        writeErrorLine(pipe, "ERROR TS3_TARGET\n");
        CloseHandle(pipe);
        return 9;
    }

    // Process-loopback can exclude only one process tree. If a second TS3
    // client is running, its stream playback would be captured and fed back
    // into the stream. Fail closed: no system audio at all rather than ever
    // capturing TeamSpeak audio.
    if (!includeTree) {
        const int initialTs3Count = countTs3Processes();
        if (initialTs3Count != 1) {
            const char errorLine[] = "ERROR MULTIPLE_TS3\n";
            DWORD written = 0;
            WriteFile(pipe, errorLine, sizeof(errorLine) - 1, &written, nullptr);
            ts3stream::diag::log(
                "audio helper: expected exactly one TS3 process, found %d; system audio disabled",
                initialTs3Count);
            CloseHandle(pipe);
            return 7;
        }
    }

    CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ts3stream::media::WasapiCapture capture;
    auto callback = [pipe](const ts3stream::media::CapturedAudioFrame& frame) {
        if (g_pipeBroken.load()) {
            return;
        }
        AudioPacketHeader header;
        header.frames = static_cast<uint32_t>(frame.frames);
        header.channels = static_cast<uint32_t>(frame.channels);
        header.sampleRate = static_cast<uint32_t>(frame.sampleRate);
        header.peak = frame.peak;
        header.timestampUs = frame.timestampUs;

        if (!writeAll(pipe, &header, sizeof(header))) {
            return;
        }
        writeAll(pipe, frame.samples.data(),
                 static_cast<DWORD>(frame.samples.size() * sizeof(int16_t)));
    };

    const bool started = capture.startProcess(targetPid, includeTree, callback);
    if (!started) {
        ts3stream::diag::log("audio helper capture start failed: %s", capture.lastError().c_str());
        CloseHandle(pipe);
        CoUninitialize();
        return 6;
    }
    ts3stream::diag::log("audio helper capture started (pid=%lu includeTree=%d)",
                         targetPid, includeTree ? 1 : 0);

    bool targetExited = false;
    bool multipleTs3 = false;
    ULONGLONG nextMonitorCheck = GetTickCount64() + 1000;

    while (!g_pipeBroken.load()) {
        Sleep(100);

        const ULONGLONG now = GetTickCount64();
        if (now < nextMonitorCheck) {
            continue;
        }
        nextMonitorCheck = now + 1000;

        // Process loopback falls back to capturing everything when the target
        // process disappears, so stop as soon as it exits.
        if (!isProcessAlive(targetPid)) {
            ts3stream::diag::log(
                "audio helper: target pid %lu exited, stopping capture", targetPid);
            writeErrorLine(pipe, "ERROR TARGET_EXITED\n");
            targetExited = true;
            break;
        }

        // A second TS3 client can start after the initial check. Keep failing
        // closed for system capture so TeamSpeak audio is never captured.
        if (!includeTree) {
            const int ts3Count = countTs3Processes();
            if (ts3Count != 1) {
                ts3stream::diag::log(
                    "audio helper: TS3 process count %d != 1, stopping system audio",
                    ts3Count);
                writeErrorLine(pipe, "ERROR MULTIPLE_TS3\n");
                multipleTs3 = true;
                break;
            }
        }
    }

    capture.stop();
    CloseHandle(pipe);
    CoUninitialize();

    if (multipleTs3) {
        return 7;
    }
    if (targetExited) {
        return 8;
    }
    return 0;
}
