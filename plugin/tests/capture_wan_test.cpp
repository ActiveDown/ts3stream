#include "media/livekit_engine.h"
#include "log.h"
#include <QAbstractVideoSurface>
#include <QApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMediaPlayer>
#include <QPainter>
#include <QScreen>
#include <QTimer>
#include <QVideoSurfaceFormat>
#include <QWidget>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <memory>
#include <mutex>
#include <vector>

// Explicit opt-in interactive diagnostic. Only this window is captured. Uses
// production WGC, encoder input queue and LiveKitEngine; never attaches to TS3.
namespace {
double percentile(std::vector<double> values, double p) {
    if (values.empty()) return -1;
    std::sort(values.begin(), values.end());
    return values[static_cast<size_t>((values.size() - 1) * p)];
}
struct Surface : QAbstractVideoSurface {
    QImage image;
    int frames = 0;
    QList<QVideoFrame::PixelFormat> supportedPixelFormats(QAbstractVideoBuffer::HandleType type) const override {
        if (type != QAbstractVideoBuffer::NoHandle) return {};
        return {QVideoFrame::Format_RGB32, QVideoFrame::Format_ARGB32, QVideoFrame::Format_RGB24};
    }
    bool present(const QVideoFrame& incoming) override {
        QVideoFrame frame(incoming);
        if (!frame.map(QAbstractVideoBuffer::ReadOnly)) return false;
        const auto format = QVideoFrame::imageFormatFromPixelFormat(frame.pixelFormat());
        if (format == QImage::Format_Invalid) { frame.unmap(); return false; }
        image = QImage(frame.bits(), frame.width(), frame.height(), frame.bytesPerLine(), format).copy();
        if (surfaceFormat().scanLineDirection() == QVideoSurfaceFormat::BottomToTop) image = image.mirrored();
        frame.unmap();
        ++frames;
        return true;
    }
};
struct SourceWindow : QWidget {
    Surface& surface;
    QElapsedTimer& clock;
    std::unique_ptr<std::atomic<qint64>[]> painted{new std::atomic<qint64>[65536]{}};
    unsigned id = 0;
    SourceWindow(Surface& surface, QElapsedTimer& clock)
        : QWidget(nullptr, Qt::Window | Qt::FramelessWindowHint), surface(surface), clock(clock) {
        setWindowTitle("TS3Stream isolated capture test - Sintel trailer");
        setFixedSize(1920, 1080);
    }
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        p.fillRect(rect(), Qt::black);
        if (!surface.image.isNull()) p.drawImage(QRect(0, 40, 1920, 1040), surface.image);
        id = (id + 1) & 65535;
        painted[id].store(clock.nsecsElapsed() / 1000);
        // ID + complement rejects blank/stale/incorrectly sized capture pixels.
        for (int bit = 0; bit < 32; ++bit) {
            const bool on = bit < 16 ? ((id >> bit) & 1) : !((id >> (bit - 16)) & 1);
            p.fillRect(bit * 32, 0, 32, 32, on ? QColor(240, 240, 240) : QColor(16, 16, 16));
        }
        p.setPen(Qt::white);
        p.drawText(1050, 24, QString("Real window capture 1080p/30 | frame %1").arg(id));
    }
};
int frameId(int w, int h, const uint8_t* pixels, int stride) {
    // MaintainFramerate may reduce encoded resolution under congestion. Read
    // the source-space marker proportionally rather than calling 720p corrupt.
    if (w < 480 || h < 270 || !pixels || stride < w * 4 || std::abs(w * 9 - h * 16) > 32) return -1;
    unsigned id = 0, inverse = 0;
    const int y = 16 * h / 1080;
    for (int bit = 0; bit < 16; ++bit) {
        if (pixels[y * stride + ((bit * 32 + 16) * w / 1920) * 4] > 128) id |= 1u << bit;
        if (pixels[y * stride + (((bit + 16) * 32 + 16) * w / 1920) * 4] > 128) inverse |= 1u << bit;
    }
    return (id ^ inverse) == 65535 ? static_cast<int>(id) : -1;
}
int markerSelfTest() {
    for (int id : {0, 1, 2, 3, 255, 256, 1023, 65535}) {
        QImage image(1920, 1080, QImage::Format_ARGB32);
        image.fill(Qt::black);
        QPainter painter(&image);
        for (int bit = 0; bit < 32; ++bit) {
            const bool on = bit < 16 ? ((id >> bit) & 1) : !((id >> (bit - 16)) & 1);
            painter.fillRect(bit * 32, 0, 32, 32, on ? QColor(240, 240, 240) : QColor(16, 16, 16));
        }
        painter.end();
        for (int width : {480, 640, 960, 1280, 1920, 2560}) {
            const auto scaled = image.scaled(width, width * 9 / 16, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
            if (frameId(scaled.width(), scaled.height(), scaled.bits(), scaled.bytesPerLine()) != id) return 8;
        }
        image.fill(Qt::black);
        if (frameId(image.width(), image.height(), image.bits(), image.bytesPerLine()) != -1) return 9;
    }
    std::printf("Visual marker: all IDs and six resolutions passed; blank frames rejected\n");
    return 0;
}
struct ReceiverStats {
    std::mutex mutex;
    int lastId = -1, bad = 0, badWarmup = 0;
    double lastArrival = 0;
    std::vector<double> delays, gaps;
    QJsonArray stalls;
    QJsonArray invalidSamples;
    QJsonObject resolutions;
};
}

int main(int argc, char** argv) {
    QApplication::setAttribute(Qt::AA_DisableHighDpiScaling);
    QApplication app(argc, argv);
    if (argc != 2) return 2;
    if (std::string(argv[1]) == "--marker-self-test") return markerSelfTest();
    QFile file(QString::fromLocal8Bit(argv[1]));
    if (!file.open(QIODevice::ReadOnly)) return 2;
    const auto config = QJsonDocument::fromJson(file.readAll()).object();
    const int durationMs = std::clamp(config.value("durationMs").toInt(90000), 15000, 300000);
    const int warmupMs = std::clamp(config.value("warmupMs").toInt(10000), 3000, durationMs / 2);
    auto value = [&](const char* key) { return config.value(key).toString().toStdString(); };
    const QString movie = config.value("movie").toString();
    if (!QFile::exists(movie) || app.primaryScreen()->geometry().width() < 1920 ||
        app.primaryScreen()->geometry().height() < 1080) {
        std::fprintf(stderr, "Movie missing or desktop smaller than 1920x1080\n"); return 77;
    }
    ts3stream::diag::initLog();
    QElapsedTimer clock;
    clock.start();
    Surface surface;
    SourceWindow source(surface, clock);
    source.move(app.primaryScreen()->geometry().topLeft());
    QMediaPlayer player;
    player.setVideoOutput(&surface);
    player.setMuted(true);
    player.setMedia(QUrl::fromLocalFile(movie));
    bool mediaError = false;
    QString mediaErrorText;
    QObject::connect(&player, QOverload<QMediaPlayer::Error>::of(&QMediaPlayer::error), &app,
        [&](QMediaPlayer::Error code) {
            if (code == QMediaPlayer::NoError) return;
            mediaError = true;
            mediaErrorText = player.errorString();
            std::fprintf(stderr, "Movie error %d: %s\n", int(code), mediaErrorText.toUtf8().constData());
            app.quit();
        });
    QObject::connect(&player, &QMediaPlayer::mediaStatusChanged, &app, [&](QMediaPlayer::MediaStatus status) {
        if (status == QMediaPlayer::EndOfMedia) { player.setPosition(0); player.play(); }
    });
    if (config.value("probeOnly").toBool()) {
        source.show(); player.play();
        QTimer paint;
        QObject::connect(&paint, &QTimer::timeout, &source, QOverload<>::of(&QWidget::update));
        paint.start(33);
        QTimer::singleShot(5000, &app, &QApplication::quit);
        app.exec();
        std::printf("Movie probe: frames=%d error=%d\n", surface.frames, int(mediaError));
        player.stop();
        return !mediaError && surface.frames > 50 ? 0 : 7;
    }
    ts3stream::media::LiveKitEngine publisher;
    std::array<ts3stream::media::LiveKitEngine, 2> viewers;
    std::array<ReceiverStats, 2> stats;
    std::string error;
    const int selectedBitrateKbps = config.value("selectedBitrateKbps").toInt();
    if (!publisher.connect(value("url"), value("publisherToken"), false, error) ||
        !publisher.ensureVideo(1920, 1080, 30, error, selectedBitrateKbps)) {
        std::fprintf(stderr, "Publisher connect: %s\n", error.c_str()); return 3;
    }
    for (int i = 0; i < 2; ++i) {
        viewers[i].setVideoFrameCallback([&, i](int w, int h, const uint8_t* pixels, int stride, int64_t) {
            const int id = frameId(w, h, pixels, stride);
            auto& s = stats[i];
            std::lock_guard<std::mutex> lock(s.mutex);
            const double now = clock.nsecsElapsed() / 1e6;
            const QString size = QString("%1x%2").arg(w).arg(h);
            s.resolutions[size] = s.resolutions[size].toInt() + 1;
            if (id < 0) {
                if (now < warmupMs) ++s.badWarmup;
                else ++s.bad;
                if (s.invalidSamples.size() < 30) s.invalidSamples.append(QJsonObject{
                    {"atMs", now}, {"width", w}, {"height", h}});
                return;
            }
            if (id == s.lastId) return;
            s.lastId = id;
            const double sent = source.painted[id].load() / 1000.0;
            if (sent < warmupMs || sent > durationMs) { s.lastArrival = 0; return; }
            if (s.lastArrival) {
                const double gap = now - s.lastArrival;
                s.gaps.push_back(gap);
                if (gap > 200) s.stalls.append(QJsonObject{{"atMs", now}, {"gapMs", gap}, {"id", id}});
            }
            s.lastArrival = now;
            s.delays.push_back(now - sent);
        });
        if (!viewers[i].connect(value("url"), value(i ? "viewer2Token" : "viewerToken"), true, error, value("targetIdentity")) ||
            viewers[i].playbackDelayMs() != 0) {
            std::fprintf(stderr, "Viewer connect/zero client delay: %s\n", error.c_str()); return 4;
        }
    }
    // Reset before showing the source; painted IDs share this clock thereafter.
    clock.restart();
    source.show();
    player.play();
    QTimer paintTimer;
    paintTimer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&paintTimer, &QTimer::timeout, &source, QOverload<>::of(&QWidget::update));
    paintTimer.start(33);
    ts3stream::media::WgcCapture capture;
    capture.setTargetFps(30);
    std::atomic<int> captured{0}, invalidCapture{0};
    int lastCaptured = -1;
    std::vector<double> captureGaps;
    double previousCapture = 0;
    bool captureOk = true;
    QTimer::singleShot(1000, &app, [&] {
        captureOk = capture.startWindow(reinterpret_cast<HWND>(source.winId()), [&](ts3stream::media::CapturedVideoFrame frame) {
            const int id = frameId(frame.width, frame.height, frame.bgra.data(), frame.stride);
            if (id < 0 || frame.width != 1920 || frame.height != 1080) { ++invalidCapture; return; }
            if (id == lastCaptured) return;
            lastCaptured = id;
            const double sent = source.painted[id].load() / 1000.0;
            const double now = clock.nsecsElapsed() / 1e6;
            if (sent >= warmupMs && sent <= durationMs) {
                ++captured;
                if (previousCapture) captureGaps.push_back(now - previousCapture);
                previousCapture = now;
            }
            publisher.pushVideo(std::move(frame));
        });
        if (!captureOk) app.quit();
    });
    QTimer::singleShot(durationMs, &app, [&] { paintTimer.stop(); player.pause(); capture.stop(); });
    QTimer::singleShot(durationMs + 4000, &app, &QApplication::quit);
    app.exec();
    capture.stop(); paintTimer.stop(); player.stop(); source.hide();
    for (auto& viewer : viewers) viewer.disconnect();
    publisher.disconnect();
    QJsonObject result{{"captureFrames", captured.load()}, {"invalidCapture", invalidCapture.load()},
        {"selectedBitrateKbps", selectedBitrateKbps},
        {"movieFrames", surface.frames}, {"mediaError", mediaError}, {"mediaErrorText", mediaErrorText},
        {"captureOk", captureOk}, {"captureError", QString::fromStdString(capture.lastError())},
        {"durationMs", durationMs}, {"warmupMs", warmupMs},
        {"captureGapP95Ms", percentile(captureGaps, .95)}, {"captureGapMaxMs", percentile(captureGaps, 1)}};
    bool passed = captureOk && !mediaError && surface.frames > durationMs / 100 && captured > (durationMs - warmupMs) / 50 && invalidCapture == 0;
    QJsonArray receiverResults;
    for (auto& s : stats) {
        std::lock_guard<std::mutex> lock(s.mutex);
        const double median = percentile(s.delays, .5), p95 = percentile(s.delays, .95), gap = percentile(s.gaps, 1);
        const bool ok = s.bad == 0 && s.delays.size() > captured * .85 && median >= 1900 && p95 < 2500 && gap < 250;
        receiverResults.append(QJsonObject{{"frames", static_cast<int>(s.delays.size())}, {"invalid", s.bad},
            {"invalidDuringWarmup", s.badWarmup}, {"resolutions", s.resolutions},
            {"delayMedianMs", median}, {"delayP95Ms", p95}, {"gapP95Ms", percentile(s.gaps, .95)},
            {"gapMaxMs", gap}, {"stalls", s.stalls}, {"invalidSamples", s.invalidSamples}, {"pass", ok}});
        passed = passed && ok;
    }
    result["viewers"] = receiverResults;
    result["pass"] = passed;
    const auto bytes = QJsonDocument(result).toJson();
    std::fwrite(bytes.data(), 1, bytes.size(), stdout);
    QFile output(config.value("resultFile").toString());
    if (!output.open(QIODevice::WriteOnly) || output.write(bytes) != bytes.size()) return 6;
    return passed ? 0 : 5;
}
