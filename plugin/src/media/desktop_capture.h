#pragma once

#include "wgc_capture.h"

namespace ts3stream::media {

// Borderless full-monitor capture for Windows versions without WGC border control.
// Window capture deliberately remains WGC: duplicating/cropping a monitor would
// expose covering windows and cannot capture an occluded application correctly.
class DesktopCapture {
public:
    using FrameCallback = WgcCapture::FrameCallback;
    DesktopCapture();
    ~DesktopCapture();
    DesktopCapture(const DesktopCapture&) = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;

    bool startMonitor(HMONITOR monitor, FrameCallback callback);
    void setTargetFps(int fps);
    void stop();
    bool isRunning() const;
    std::string lastError() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ts3stream::media
