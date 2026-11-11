#include "media/wgc_capture.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace {

BOOL CALLBACK monitorCallback(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    auto* monitors = reinterpret_cast<std::vector<HMONITOR>*>(data);
    monitors->push_back(monitor);
    return TRUE;
}

std::wstring monitorName(HMONITOR monitor) {
    MONITORINFOEXW info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info)) {
        return info.szDevice;
    }
    return L"<monitor>";
}

} // namespace

int main() {
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, monitorCallback, reinterpret_cast<LPARAM>(&monitors));

    std::wprintf(L"Monitors: %zu\n", monitors.size());
    if (monitors.empty()) {
        std::printf("No monitors found\n");
        return 1;
    }

    for (size_t i = 0; i < monitors.size(); ++i) {
        const auto name = monitorName(monitors[i]);
        std::wprintf(L"  [%zu] %s\n", i, name.c_str());
    }

    ts3stream::media::WgcCapture capture;
    std::atomic<int> frames{0};
    std::atomic<int> width{0};
    std::atomic<int> height{0};
    std::atomic<int64_t> firstTimestamp{0};

    const bool started = capture.startMonitor(monitors[0], [&](const ts3stream::media::CapturedVideoFrame& frame) {
        if (frames.fetch_add(1) == 0) {
            width.store(frame.width);
            height.store(frame.height);
            firstTimestamp.store(frame.timestampUs);
        }
    });

    if (!started) {
        std::printf("WGC start failed: %s\n", capture.lastError().c_str());
        return 2;
    }

    std::printf("Capture started, waiting 5 seconds...\n");
    std::this_thread::sleep_for(std::chrono::seconds(5));
    capture.stop();

    std::printf("Frames: %d, size: %dx%d, firstTimestampUs: %lld\n",
                frames.load(), width.load(), height.load(),
                static_cast<long long>(firstTimestamp.load()));

    return frames.load() > 0 ? 0 : 3;
}
