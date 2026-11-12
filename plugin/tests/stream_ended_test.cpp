#include "media/livekit_engine.h"
#include "log.h"
#include "ui/fullscreen_window.h"
#include "ui/stream_panel.h"

#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTimer>

#include <atomic>
#include <cstdio>
#include <array>

// Integration test against an isolated LiveKit room. The JSON file contains
// url, targetIdentity, publisherToken, otherToken and viewerToken. Never commit it.
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    ts3stream::diag::initLog();
    app.setQuitOnLastWindowClosed(false);
    if (argc != 3 && argc != 4) return 2;
    const bool staticSource = argc == 4 && std::string(argv[3]) == "static";
    const bool resizeSource = argc == 4 && std::string(argv[3]) == "resize";
    const std::array<QSize, 8> resizeSizes = {QSize(640, 348), QSize(640, 360), QSize(1280, 720),
        QSize(640, 348), QSize(1920, 1044), QSize(1920, 1080), QSize(2560, 1440), QSize(1920, 1044)};
    int resizeStage = 0;
    std::atomic<int> receivedStage{-1};
    QFile file(QString::fromLocal8Bit(argv[1]));
    if (!file.open(QIODevice::ReadOnly)) return 2;
    const auto config = QJsonDocument::fromJson(file.readAll()).object();
    auto value = [&](const char* key) { return config.value(key).toString().toStdString(); };
    const int mode = std::atoi(argv[2]);
    QWidget host;
    host.resize(800, 600);
    host.show();
    QPointer<ts3stream::ui::StreamPanel> panel = new ts3stream::ui::StreamPanel(&host);
    panel->setGeometry(host.rect());
    panel->show();
    if (mode == 1) ts3stream::ui::showDetachedWindow(panel);
    if (mode == 2) ts3stream::ui::showFullscreenOnCurrentMonitor(panel);

    ts3stream::media::LiveKitEngine publisher, other, viewer;
    std::string error;
    if (!publisher.connect(value("url"), value("publisherToken"), false, error) ||
        !other.connect(value("url"), value("otherToken"), false, error)) {
        std::fprintf(stderr, "publisher connect: %s\n", error.c_str());
        return 3;
    }
    std::atomic<int> frames{0};
    int ended = 0;
    viewer.setVideoFrameCallback([&](int width, int height, const uint8_t* data, int stride, int64_t) {
        ++frames;
        if (!resizeSource) return;
        const auto* pixel = data + (height / 2) * stride + (width / 2) * 4;
        for (int i = 0; i < static_cast<int>(resizeSizes.size()); ++i) {
            const int gray = 32 + i * 24;
            if (QSize(width, height) == resizeSizes[i] &&
                std::abs(int(pixel[0]) - gray) < 8 && std::abs(int(pixel[1]) - gray) < 8 &&
                std::abs(int(pixel[2]) - gray) < 8) receivedStage.store(i);
        }
    });
    viewer.setStreamEndedCallback([&] {
        QMetaObject::invokeMethod(&app, [&] {
            ++ended;
            viewer.disconnect();
            if (panel) {
                panel->hide();
                panel->deleteLater();
            }
        }, Qt::QueuedConnection);
    });
    const QSize initialSize = resizeSource ? resizeSizes[0] : QSize(640, 360);
    if (!publisher.ensureVideo(initialSize.width(), initialSize.height(), 30, error)) {
        std::fprintf(stderr, "viewer/video setup: %s\n", error.c_str());
        return 4;
    }
    QElapsedTimer clock;
    clock.start();
    auto sendFrame = [&] {
        ts3stream::media::CapturedVideoFrame frame;
        const QSize size = resizeSource ? resizeSizes[resizeStage] : QSize(640, 360);
        frame.width = size.width();
        frame.height = size.height();
        frame.stride = size.width() * 4;
        frame.timestampUs = clock.nsecsElapsed() / 1000;
        frame.bgra.assign(static_cast<size_t>(frame.stride) * frame.height,
                          resizeSource ? 32 + resizeStage * 24 : 128);
        publisher.pushVideo(std::move(frame));
    };
    if (staticSource) sendFrame();
    // Late subscriber to an unchanged WGC window: only ONE source frame is
    // ever supplied, before the viewer joins. No repaint may be required.
    QTimer::singleShot(staticSource ? 1500 : 0, &app, [&] {
        if (!viewer.connect(value("url"), value("viewerToken"), true, error, value("targetIdentity"))) {
            std::fprintf(stderr, "viewer connect: %s\n", error.c_str());
            app.exit(4);
        }
    });
    qint64 targetStoppedAt = 0;
    int phase = 0;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (clock.elapsed() > (resizeSource ? 45000 : 15000)) {
            std::printf("timeout mode=%d frames=%d ended=%d resizeStage=%d receivedStage=%d\n",
                        mode, frames.load(), ended, resizeStage, receivedStage.load());
            app.exit(5);
            return;
        }
        if (phase < 2 && !staticSource) sendFrame();
        if (resizeSource && phase == 0) {
            if (ended != 0 || !panel || !panel->isVisible()) {
                std::printf("resize closed viewer FAIL\n");
                app.exit(8);
                return;
            }
            if (receivedStage.load() != resizeStage) return;
            std::printf("resize stage=%d received=%dx%d with fresh content PASS\n", resizeStage,
                        resizeSizes[resizeStage].width(), resizeSizes[resizeStage].height());
            if (resizeStage + 1 < static_cast<int>(resizeSizes.size())) {
                ++resizeStage;
                return;
            }
        }
        if (phase == 0 && frames.load() > 0) {
            other.disconnect();
            targetStoppedAt = clock.elapsed();
            phase = 1;
        } else if (phase == 1 && clock.elapsed() - targetStoppedAt > 1000) {
            if (ended != 0 || !panel || !panel->isVisible()) {
                std::printf("unrelated participant closed viewer FAIL\n");
                app.exit(6);
                return;
            }
            publisher.disconnect();
            targetStoppedAt = clock.elapsed();
            phase = 2;
        } else if (phase == 2 && clock.elapsed() - targetStoppedAt > 1000) {
            const bool ok = ended == 1 && panel.isNull() && host.isVisible();
            std::printf("mode=%d frames=%d ended=%d viewerDeleted=%d hostVisible=%d %s\n",
                        mode, frames.load(), ended, panel.isNull(), host.isVisible(), ok ? "PASS" : "FAIL");
            app.exit(ok ? 0 : 7);
        }
    });
    timer.start(33);
    const int result = app.exec();
    viewer.disconnect();
    other.disconnect();
    publisher.disconnect();
    ts3stream::media::LiveKitEngine::shutdownSdk();
    return result;
}
