#include "audio_pipe_client.h"

#include "../log.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <thread>

namespace ts3stream::media {
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

std::wstring helperPath() {
    const std::wstring appDataPath = diag::dataDir() + L"\\ts3stream-audio.exe";
    const std::wstring moduleDir = diag::moduleDir();
    const std::wstring candidates[] = {
        moduleDir + L"\\ts3stream\\ts3stream-audio.exe",
        moduleDir + L"\\ts3stream-audio.exe",
        appDataPath,
    };
    for (const auto& candidate : candidates) {
        if (!candidate.empty() &&
            GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES) {
            return candidate;
        }
    }
    return appDataPath;
}

} // namespace

struct RemoteAudioCapture::Impl {
    HANDLE pipe = INVALID_HANDLE_VALUE;
    HANDLE process = nullptr;
    std::thread reader;
    std::atomic<bool> running{false};
    Callback callback;
    std::string error;

    ~Impl() {
        stop();
    }

    bool readExact(void* data, DWORD size) {
        auto* bytes = static_cast<uint8_t*>(data);
        DWORD read = 0;
        while (read < size) {
            DWORD chunk = 0;
            if (!ReadFile(pipe, bytes + read, size - read, &chunk, nullptr) || chunk == 0) {
                return false;
            }
            read += chunk;
        }
        return true;
    }

    void readLoop() {
        while (running.load()) {
            AudioPacketHeader header;
            if (!readExact(&header, sizeof(header))) {
                error = "audio helper stopped";
                break;
            }
            if (header.magic != kMagic || header.frames == 0 || header.channels == 0 ||
                header.channels > 8 || header.sampleRate == 0 ||
                header.frames > header.sampleRate * 2) {
                diag::log("RemoteAudioCapture: invalid packet header");
                break;
            }

            CapturedAudioFrame frame;
            frame.frames = static_cast<int>(header.frames);
            frame.channels = static_cast<int>(header.channels);
            frame.sampleRate = static_cast<int>(header.sampleRate);
            frame.peak = header.peak;
            frame.timestampUs = header.timestampUs;
            frame.samples.resize(static_cast<size_t>(header.frames) * header.channels);

            const DWORD payloadSize = static_cast<DWORD>(frame.samples.size() * sizeof(int16_t));
            if (!readExact(frame.samples.data(), payloadSize)) {
                error = "audio helper payload read failed";
                break;
            }
            if (running.load() && callback) {
                callback(frame);
            }
        }

        running.store(false);
    }

    bool start(const std::string& command, Callback cb) {
        stop();
        callback = std::move(cb);
        error.clear();

        const std::wstring path = helperPath();
        if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
            error = "audio helper not found";
            return false;
        }

        wchar_t pipeName[256] = {};
        std::swprintf(pipeName, std::size(pipeName), L"\\\\.\\pipe\\ts3stream-audio-%lu-%llu",
                      GetCurrentProcessId(), static_cast<unsigned long long>(GetTickCount64()));

        std::wstring commandLine = L"\"" + path + L"\" \"" + pipeName + L"\"";
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION processInfo{};
        std::wstring mutableCommand = commandLine;
        if (!CreateProcessW(path.c_str(), mutableCommand.data(), nullptr, nullptr, FALSE,
                            CREATE_NO_WINDOW, nullptr, nullptr, &startup, &processInfo)) {
            error = "CreateProcess for audio helper failed";
            return false;
        }
        process = processInfo.hProcess;
        CloseHandle(processInfo.hThread);

        // Wait for the helper to create its named pipe, then connect.
        for (int i = 0; i < 60; ++i) {
            pipe = CreateFileW(pipeName, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                               OPEN_EXISTING, 0, nullptr);
            if (pipe != INVALID_HANDLE_VALUE) {
                break;
            }
            Sleep(50);
        }
        if (pipe == INVALID_HANDLE_VALUE) {
            error = "audio helper pipe connection failed";
            stop();
            return false;
        }

        const std::string line = command + "\n";
        DWORD written = 0;
        if (!WriteFile(pipe, line.data(), static_cast<DWORD>(line.size()), &written, nullptr) ||
            written != line.size()) {
            error = "audio helper command write failed";
            stop();
            return false;
        }

        running.store(true);
        reader = std::thread([this] { readLoop(); });
        diag::log("RemoteAudioCapture started: %s", command.c_str());
        return true;
    }

    void stop() {
        running.store(false);
        if (pipe != INVALID_HANDLE_VALUE) {
            CloseHandle(pipe);
            pipe = INVALID_HANDLE_VALUE;
        }
        if (reader.joinable()) {
            reader.join();
        }
        if (process) {
            WaitForSingleObject(process, 300);
            DWORD exitCode = 0;
            if (GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE) {
                TerminateProcess(process, 0);
            }
            CloseHandle(process);
            process = nullptr;
        }
        callback = nullptr;
    }
};

RemoteAudioCapture::RemoteAudioCapture() : impl_(std::make_unique<Impl>()) {}
RemoteAudioCapture::~RemoteAudioCapture() = default;

bool RemoteAudioCapture::startSystemExcludingTs3(DWORD ts3ProcessId, Callback callback) {
    return impl_->start("SYSTEM " + std::to_string(ts3ProcessId), std::move(callback));
}

bool RemoteAudioCapture::startApp(DWORD processId, Callback callback) {
    return impl_->start("APP " + std::to_string(processId), std::move(callback));
}

void RemoteAudioCapture::stop() {
    impl_->stop();
}

bool RemoteAudioCapture::isRunning() const {
    return impl_->running.load();
}

std::string RemoteAudioCapture::lastError() const {
    return impl_->error;
}

} // namespace ts3stream::media
