#include "media/desktop_capture.h"
#include "log.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QPoint>
#include <QTimer>
#include <atomic>
#include <cstdio>

// A native fixture avoids Qt retaining the previous monitor's logical size
// during WM_DPICHANGED while SetWindowPos already uses the new physical size.
class TestWindow {
public:
    int gray = 0;
    int tick = 0;
    HWND handle = nullptr;
    TestWindow() {
        WNDCLASSW cls{};
        cls.lpfnWndProc = windowProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"TS3StreamDesktopCaptureTest";
        RegisterClassW(&cls);
        handle = CreateWindowExW(WS_EX_TOPMOST | WS_EX_NOACTIVATE, cls.lpszClassName, L"DXGI test",
            WS_POPUP, 60, 60, 320, 240, nullptr, nullptr, cls.hInstance, this);
        ShowWindow(handle, SW_SHOWNOACTIVATE);
    }
    ~TestWindow() {
        DestroyWindow(handle);
        UnregisterClassW(L"TS3StreamDesktopCaptureTest", GetModuleHandleW(nullptr));
    }
    void update() { InvalidateRect(handle, nullptr, FALSE); }
    static LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
            const auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto* self = reinterpret_cast<TestWindow*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        if (message == WM_PAINT && self) {
            PAINTSTRUCT paint{};
            const HDC dc = BeginPaint(hwnd, &paint);
            RECT rect{};
            GetClientRect(hwnd, &rect);
            HBRUSH brush = CreateSolidBrush(RGB(self->gray, self->gray, self->gray));
            FillRect(dc, &rect, brush);
            DeleteObject(brush);
            const RECT moving{30, 30, 40, 40};
            brush = CreateSolidBrush(RGB((++self->tick * 17) % 255, 0, 0));
            FillRect(dc, &moving, brush);
            DeleteObject(brush);
            EndPaint(hwnd, &paint);
            return 0;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
};

BOOL CALLBACK addMonitor(HMONITOR monitor, HDC, LPRECT, LPARAM data) {
    reinterpret_cast<std::vector<HMONITOR>*>(data)->push_back(monitor);
    return TRUE;
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ts3stream::diag::initLog();
    std::vector<HMONITOR> monitors;
    EnumDisplayMonitors(nullptr, nullptr, addMonitor, reinterpret_cast<LPARAM>(&monitors));
    if (monitors.empty()) return 2;
    TestWindow window;
    if (!window.handle) return 2;
    ts3stream::media::DesktopCapture capture;
    if (capture.startMonitor(nullptr, {}) || capture.isRunning() || capture.lastError().empty()) return 3;
    size_t monitorIndex = 0;
    int stage = 0;
    std::atomic<int> expectedGray{0}, matched{-1}, frames{0};
    std::atomic<int> actualGray{-1};
    std::atomic<int> actualSamples[3]{};
    std::atomic<bool> invalid{false};
    std::atomic<int64_t> previousTimestamp{0};
    QElapsedTimer clock;
    clock.start();
    qint64 changedAt = 0;
    auto start = [&] {
        capture.stop();
        window.gray = 0;
        expectedGray.store(0);
        MONITORINFO info{}; info.cbSize = sizeof(info);
        if (!GetMonitorInfoW(monitors[monitorIndex], &info)) return false;
        SetWindowPos(window.handle, HWND_TOPMOST,
            info.rcMonitor.left + 60, info.rcMonitor.top + 60, 320, 240, SWP_NOACTIVATE);
        RECT windowRect{};
        GetWindowRect(window.handle, &windowRect);
        std::printf("DXGI test monitor=%zu rect=%ld,%ld-%ld,%ld window=%ld,%ld-%ld,%ld\n", monitorIndex,
            info.rcMonitor.left, info.rcMonitor.top, info.rcMonitor.right, info.rcMonitor.bottom,
            windowRect.left, windowRect.top, windowRect.right, windowRect.bottom);
        const int x = windowRect.left - info.rcMonitor.left;
        const int y = windowRect.top - info.rcMonitor.top;
        // Moving between differently scaled displays triggers WM_DPICHANGED.
        // Sample the native physical window, not its requested Qt dimensions.
        const int windowWidth = windowRect.right - windowRect.left;
        const int windowHeight = windowRect.bottom - windowRect.top;
        const int width = info.rcMonitor.right - info.rcMonitor.left;
        const int height = info.rcMonitor.bottom - info.rcMonitor.top;
        frames.store(0); matched.store(-1); previousTimestamp.store(0);
        capture.setTargetFps(30);
        return capture.startMonitor(monitors[monitorIndex], [&, x, y, width, height, windowWidth, windowHeight](ts3stream::media::CapturedVideoFrame frame) {
            ++frames;
            const auto previous = previousTimestamp.exchange(frame.timestampUs);
            if (frame.width != width || frame.height != height || frame.stride < width * 4 ||
                frame.bgra.size() < static_cast<size_t>(frame.stride) * height || frame.timestampUs <= previous) {
                invalid.store(true); return;
            }
            const int gray = expectedGray.load();
            int sampleIndex = 0;
            bool fresh = true;
            for (const QPoint point : {QPoint(x + 16, y + 16), QPoint(x + windowWidth / 2, y + windowHeight / 2),
                                       QPoint(x + windowWidth - 16, y + windowHeight - 16)}) {
                if (point.x() >= width || point.y() >= height) { invalid.store(true); return; }
                const auto* pixel = frame.bgra.data() + static_cast<size_t>(point.y()) * frame.stride + point.x() * 4;
                actualGray.store(pixel[0]);
                actualSamples[sampleIndex++].store(pixel[0]);
                // Assert a fresh black/white transition, not color-profile
                // accuracy: DXGI SDR conversion can tint the white endpoint.
                for (int c = 0; c < 3; ++c) {
                    if (gray == 0 ? pixel[c] > 16 : pixel[c] < 200) fresh = false;
                }
            }
            if (fresh) matched.store(gray);
        });
    };
    if (!start()) { std::fprintf(stderr, "DXGI start failed: %s\n", capture.lastError().c_str()); return 4; }
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        window.update();
        if (invalid.load() || clock.elapsed() - changedAt > 8000) {
            std::printf("DXGI failure monitor=%zu stage=%d frames=%d matched=%d error=%s\n",
                monitorIndex, stage, frames.load(), matched.load(), capture.lastError().c_str());
            RECT rect{};
            GetWindowRect(window.handle, &rect);
            std::printf("actualGray=%d expected=%d current window=%ld,%ld-%ld,%ld\n", actualGray.load(),
                expectedGray.load(), rect.left, rect.top, rect.right, rect.bottom);
            const HDC dc = GetDC(window.handle);
            std::printf("samples=%d,%d,%d nativePixel=%lu fixtureGray=%d\n", actualSamples[0].load(),
                actualSamples[1].load(), actualSamples[2].load(), GetPixel(dc, 16, 16), window.gray);
            ReleaseDC(window.handle, dc);
            app.exit(5); return;
        }
        if (matched.load() != expectedGray.load() || clock.elapsed() - changedAt < 500) return;
        std::printf("DXGI monitor=%zu stage=%d fresh pixels PASS frames=%d\n", monitorIndex, stage, frames.load());
        changedAt = clock.elapsed();
        if (++stage == 3) {
            capture.stop();
            if (capture.isRunning()) { app.exit(6); return; }
            if (++monitorIndex == monitors.size()) { app.exit(0); return; }
            stage = 0;
            if (!start()) { app.exit(7); return; }
        }
        // Use black/white endpoints so SDR/HDR color management on different
        // displays does not turn a freshness assertion into a gamma assertion.
        window.gray = stage % 2 ? 255 : 0;
        expectedGray.store(window.gray);
        capture.setTargetFps(stage == 1 ? 60 : 30);
    });
    timer.start(16);
    const int result = app.exec();
    capture.stop();
    std::printf("desktop capture: monitors=%zu exit=%d\n", monitors.size(), result);
    return result;
}
