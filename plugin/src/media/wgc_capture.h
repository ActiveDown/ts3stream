#pragma once

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ts3stream::media {

struct CapturedVideoFrame {
    int width = 0;
    int height = 0;
    int stride = 0;
    int64_t timestampUs = 0;
    std::vector<uint8_t> bgra;
};

class WgcCapture {
public:
    using FrameCallback = std::function<void(CapturedVideoFrame)>;

    WgcCapture();
    ~WgcCapture();

    WgcCapture(const WgcCapture&) = delete;
    WgcCapture& operator=(const WgcCapture&) = delete;

    bool startMonitor(HMONITOR monitor, FrameCallback callback);
    bool startWindow(HWND window, FrameCallback callback);
    void setTargetFps(int fps);
    void stop();

    bool isRunning() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ts3stream::media
