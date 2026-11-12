#include "media/wgc_capture.h"
#include "log.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QWidget>
#include <atomic>
#include <cstdio>

class CaptureWindow : public QWidget {
public:
    int gray = 40;
    int tick = 0;
    CaptureWindow() : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint) {}
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(gray, gray, gray));
        painter.fillRect(QRect(30, 30, 10, 10), QColor((tick * 17) % 255, 0, 0));
    }
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ts3stream::diag::initLog();
    CaptureWindow window;
    window.resize(640, 348);
    window.show();
    ts3stream::media::WgcCapture capture;
    std::atomic<int> expectedWidth{640}, expectedHeight{348}, expectedGray{40};
    std::atomic<int> matchedGray{-1};
    std::atomic<int> actualWidth{0}, actualHeight{0}, actualGray{-1};
    std::atomic<int> topLeft{-1}, bottomRight{-1};
    int stage = 0;
    QElapsedTimer clock;
    clock.start();
    qint64 changedAt = 0;
    capture.setTargetFps(30);
    if (!capture.startWindow(reinterpret_cast<HWND>(window.winId()), [&](ts3stream::media::CapturedVideoFrame frame) {
        actualWidth.store(frame.width);
        actualHeight.store(frame.height);
        actualGray.store(frame.bgra[(frame.height / 2) * frame.stride + (frame.width / 2) * 4]);
        topLeft.store(frame.bgra[16 * frame.stride + 16 * 4]);
        bottomRight.store(frame.bgra[(frame.height - 16) * frame.stride + (frame.width - 16) * 4]);
        if (frame.width != expectedWidth.load() || frame.height != expectedHeight.load()) return;
        const int gray = expectedGray.load();
        // Check fresh pixels across the entire resized surface, not only its
        // dimensions or the center of a partly copied/black frame.
        for (const QPoint point : {QPoint(16, 16), QPoint(frame.width / 2, frame.height / 2),
                                   QPoint(frame.width - 16, frame.height - 16)}) {
            const auto* pixel = frame.bgra.data() + point.y() * frame.stride + point.x() * 4;
            for (int channel = 0; channel < 3; ++channel) {
                if (std::abs(int(pixel[channel]) - gray) > 2) return;
            }
        }
        matchedGray.store(gray);
    })) {
        std::fprintf(stderr, "WGC start failed: %s\n", capture.lastError().c_str());
        return 2;
    }
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        ++window.tick;
        window.update();
        if (clock.elapsed() - changedAt > 7000) {
            std::printf("WGC resize timeout stage=%d expected=%dx%d matchedGray=%d\n",
                        stage, expectedWidth.load(), expectedHeight.load(), matchedGray.load());
            std::printf("actual=%dx%d centerGray=%d wantedGray=%d\n", actualWidth.load(), actualHeight.load(),
                        actualGray.load(), expectedGray.load());
            std::printf("topLeft=%d bottomRight=%d window=%d,%d %dx%d\n", topLeft.load(), bottomRight.load(),
                        window.x(), window.y(), window.width(), window.height());
            app.exit(3);
            return;
        }
        if (matchedGray.load() != expectedGray.load()) return;
        std::printf("WGC stage=%d size=%dx%d fresh center/corners PASS\n",
                    stage, expectedWidth.load(), expectedHeight.load());
        if (++stage == 7) { app.exit(0); return; }
        if (stage == 1 || stage == 4) {
            window.showFullScreen();
        } else {
            window.showNormal();
            const QSize size = stage == 2 ? QSize(800, 600) : stage == 3 ? QSize(1001, 777) : QSize(640, 348);
            window.resize(size);
            // WGC can black out the part of a window outside its monitor.
            // Keep pixel checks on-screen while testing buffer resizing.
            window.move(window.screen()->availableGeometry().topLeft() + QPoint(30, 30));
        }
        window.gray = 40 + stage * 25;
        expectedWidth.store(window.width());
        expectedHeight.store(window.height());
        expectedGray.store(window.gray);
        changedAt = clock.elapsed();
    });
    timer.start(33);
    const int result = app.exec();
    capture.stop();
    return result;
}
