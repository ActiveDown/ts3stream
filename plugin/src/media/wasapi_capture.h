#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ts3stream::media {

struct CapturedAudioFrame {
    std::vector<int16_t> samples;
    int sampleRate = 48000;
    int channels = 2;
    int frames = 0;
    int64_t timestampUs = 0;
    int peak = 0;
};

class WasapiCapture {
public:
    using Callback = std::function<void(const CapturedAudioFrame&)>;

    WasapiCapture();
    ~WasapiCapture();

    WasapiCapture(const WasapiCapture&) = delete;
    WasapiCapture& operator=(const WasapiCapture&) = delete;

    bool startDevice(const std::wstring& deviceId, Callback callback);
    bool startProcess(DWORD processId, bool includeProcessTree, Callback callback);
    void stop();

    bool isRunning() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ts3stream::media
