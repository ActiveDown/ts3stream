#include "media/livekit_engine.h"
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

// Integration test against an isolated LiveKit room. The JSON file contains
// url, targetIdentity, publisherToken, otherToken and viewerToken. Never commit it.
int main(int argc, char** argv) {
    QApplication app(argc, argv);
    app.setQuitOnLastWindowClosed(false);
    if (argc != 3) return 2;
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
    viewer.setVideoFrameCallback([&](int, int, const uint8_t*, int, int64_t) { ++frames; });
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
    if (!viewer.connect(value("url"), value("viewerToken"), true, error, value("targetIdentity")) ||
        !publisher.ensureVideo(640, 360, 30, error)) {
        std::fprintf(stderr, "viewer/video setup: %s\n", error.c_str());
        return 4;
    }
    QElapsedTimer clock;
    clock.start();
    qint64 targetStoppedAt = 0;
    int phase = 0;
    QTimer timer;
    QObject::connect(&timer, &QTimer::timeout, &app, [&] {
        if (clock.elapsed() > 15000) {
            std::printf("timeout mode=%d frames=%d ended=%d\n", mode, frames.load(), ended);
            app.exit(5);
            return;
        }
        if (phase < 2) {
            ts3stream::media::CapturedVideoFrame frame;
            frame.width = 640;
            frame.height = 360;
            frame.stride = 640 * 4;
            frame.timestampUs = clock.nsecsElapsed() / 1000;
            frame.bgra.assign(640 * 360 * 4, 128);
            publisher.pushVideo(std::move(frame));
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
