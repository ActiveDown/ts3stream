#include "media/livekit_engine.h"
#include "log.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTimer>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <vector>

// Isolated local-room benchmark: no screen/audio device is captured. Timestamp
// the encoded visual frame IDs and audio tone onsets on the same local clock.
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    if (argc != 2) return 2;
    ts3stream::diag::initLog();
    const QString logPath = QString::fromStdWString(ts3stream::diag::dataDir()) + "/diag.log";
    const qint64 logStart = QFile(logPath).size();
    QFile file(QString::fromLocal8Bit(argv[1]));
    if (!file.open(QIODevice::ReadOnly)) return 2;
    const auto config = QJsonDocument::fromJson(file.readAll()).object();
    const int expectedDelayMs = config.value("expectedDelayMs").toInt();
    const int expectedClientDelayMs = config.value("expectedClientDelayMs").toInt(expectedDelayMs);
    const int viewerStartMs = config.value("viewerStartMs").toInt();
    const int reconnectAtMs = config.value("reconnectAtMs").toInt();
    const int warmupMs = std::clamp(config.value("warmupMs").toInt(), 0, 300000);
    auto value = [&](const char* key) { return config.value(key).toString().toStdString(); };
    ts3stream::media::LiveKitEngine publisher, other, viewer;
    std::string error;
    if (!publisher.connect(value("url"), value("publisherToken"), false, error) ||
        !other.connect(value("url"), value("otherToken"), false, error) ||
        !other.ensureVideo(640, 360, 30, error)) {
        std::fprintf(stderr, "connect: %s\n", error.c_str()); return 3;
    }
    QElapsedTimer clock;
    clock.start();
    std::array<std::atomic<qint64>, 1024> sentAt{};
    std::mutex mutex;
    std::vector<double> videoDelays;
    std::array<double, 5> audioSent{}, audioReceived{};
    constexpr std::array<int, 5> frequencies{600, 900, 1200, 1500, 1800};
    std::array<bool, 6> scenes{};
    double previousVideo = 0, maxGap = 0, steadyGap = 0;
    std::atomic<double> lastConnectMs{0};
    int previousId = -1;
    size_t receivedFrames = 0;
    std::atomic<int> invalid{0};
    viewer.setVideoFrameCallback([&](int width, int height, const uint8_t* pixels, int stride, int64_t) {
        if (width != 1920 || height != 1080) { ++invalid; return; }
        int id = 0;
        for (int bit = 0; bit < 10; ++bit) {
            if (pixels[16 * stride + (bit * 32 + 16) * 4] > 128) id |= 1 << bit;
        }
        const qint64 sent = sentAt[id].load();
        if (!sent) return;
        const double now = clock.nsecsElapsed() / 1e6;
        std::lock_guard<std::mutex> lock(mutex);
        if (id == previousId) return;
        if (receivedFrames++ < 5 || (previousVideo && now - previousVideo > 200)) {
            std::printf("video sample id=%d previous=%d arrival_ms=%.1f delay_ms=%.1f gap_ms=%.1f\n",
                id, previousId, now, now - sent / 1000.0, previousVideo ? now - previousVideo : 0);
        }
        previousId = id;
        if (previousVideo) maxGap = std::max(maxGap, now - previousVideo);
        if (previousVideo > lastConnectMs.load() + expectedDelayMs + 2000) {
            steadyGap = std::max(steadyGap, now - previousVideo);
        }
        previousVideo = now;
        if (now >= warmupMs) videoDelays.push_back(now - sent / 1000.0);
        scenes[std::min(5, id / 60)] = true;
    });
    viewer.setAudioFrameCallback([&](const int16_t* samples, int frames, int channels, int rate) {
        double sum = 0;
        for (int i = 0; i < frames * channels; ++i) sum += double(samples[i]) * samples[i];
        if (!frames || !channels || std::sqrt(sum / (frames * channels)) < 3000) return;
        // Decode pulse IDs: a late join/reconnect must not pair a new pulse
        // with a lost earlier pulse and accidentally hide A/V drift.
        int pulse = -1;
        double best = 0, second = 0;
        for (int k = 0; k < 5; ++k) {
            const double coefficient = 2 * std::cos(6.283185307179586 * frequencies[k] / rate);
            double a = 0, b = 0;
            for (int i = 0; i < frames; ++i) {
                const double next = samples[i * channels] + coefficient * a - b;
                b = a; a = next;
            }
            const double power = a * a + b * b - coefficient * a * b;
            if (power > best) { second = best; best = power; pulse = k; }
            else second = std::max(second, power);
        }
        if (pulse < 0 || best < second * 2) return;
        const double now = clock.nsecsElapsed() / 1e6;
        std::lock_guard<std::mutex> lock(mutex);
        if (!audioReceived[pulse]) {
            audioReceived[pulse] = now;
            std::printf("audio pulse=%d arrival_ms=%.1f delay_ms=%.1f\n", pulse, now, now - audioSent[pulse]);
        }
    });
    bool connectionOk = true;
    auto connectViewer = [&] {
        lastConnectMs.store(clock.nsecsElapsed() / 1e6);
        if (!viewer.connect(value("url"), value("viewerToken"), true, error, value("targetIdentity")) ||
            viewer.playbackDelayMs() != expectedClientDelayMs) {
            std::fprintf(stderr, "viewer/delay contract: %s actual_delay=%d\n", error.c_str(), viewer.playbackDelayMs());
            connectionOk = false;
        }
    };
    // Viewer joins before the target publishes; the non-target is already live.
    if (viewerStartMs == 0) connectViewer();
    if (!connectionOk || !publisher.ensureVideo(1920, 1080, 30, error) || !publisher.ensureAudio(error)) {
        std::fprintf(stderr, "viewer/publish: %s\n", error.c_str()); return 4;
    }
    std::array<std::vector<uint8_t>, 6> pictures;
    for (unsigned scene = 0; scene < pictures.size(); ++scene) {
        auto& pixels = pictures[scene]; pixels.resize(1920 * 1080 * 4);
        for (int y = 0; y < 1080; ++y) for (int x = 0; x < 1920; ++x) {
            const unsigned pattern = ((x / 8 + scene * 17) * 2654435761u) ^ ((y / 8 + scene * 23) * 2246822519u);
            const size_t at = (static_cast<size_t>(y) * 1920 + x) * 4;
            pixels[at] = static_cast<uint8_t>(pattern);
            pixels[at + 1] = static_cast<uint8_t>(pattern >> 8);
            pixels[at + 2] = static_cast<uint8_t>(pattern >> 16);
            pixels[at + 3] = 255;
        }
    }
    int id = 0;
    int sentFrames = 0;
    QTimer videoTimer;
    videoTimer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&videoTimer, &QTimer::timeout, &app, [&] {
        if (sentFrames >= 360 + warmupMs / 33) { videoTimer.stop(); return; }
        id = sentFrames % 360;
        ts3stream::media::CapturedVideoFrame frame;
        frame.width = 1920; frame.height = 1080; frame.stride = 1920 * 4;
        frame.bgra = pictures[id / 60];
        for (int bit = 0; bit < 10; ++bit) for (int y = 0; y < 32; ++y) for (int x = bit * 32; x < (bit + 1) * 32; ++x) {
            const size_t at = static_cast<size_t>(y) * frame.stride + x * 4;
            for (int c = 0; c < 3; ++c) frame.bgra[at + c] = (id & (1 << bit)) ? 240 : 16;
        }
        // Motion continues between the full-scene replacements.
        const int marker = 400 + (id % 60) * 20;
        for (int y = 80; y < 160; ++y) for (int x = marker; x < marker + 32; ++x) {
            const size_t at = static_cast<size_t>(y) * frame.stride + x * 4;
            frame.bgra[at] = 255; frame.bgra[at + 1] = 0; frame.bgra[at + 2] = 0;
        }
        frame.timestampUs = clock.nsecsElapsed() / 1000;
        sentAt[id].store(frame.timestampUs);
        publisher.pushVideo(std::move(frame));
        // A different live video must never be downloaded by the selected viewer.
        ts3stream::media::CapturedVideoFrame unwanted;
        unwanted.width = 640; unwanted.height = 360; unwanted.stride = 640 * 4;
        unwanted.bgra.assign(640 * 360 * 4, 200);
        other.pushVideo(std::move(unwanted));
        ++sentFrames;
    });
    int audioTick = -warmupMs / 20;
    QTimer audioTimer;
    audioTimer.setTimerType(Qt::PreciseTimer);
    QObject::connect(&audioTimer, &QTimer::timeout, &app, [&] {
        const bool tone = audioTick >= 50 && audioTick < 550 && ((audioTick - 50) % 100) < 5;
        if (tone && ((audioTick - 50) % 100) == 0) {
            std::lock_guard<std::mutex> lock(mutex);
            audioSent[(audioTick - 50) / 100] = clock.nsecsElapsed() / 1e6;
        }
        ts3stream::media::CapturedAudioFrame frame;
        frame.sampleRate = 48000; frame.channels = 2; frame.samples.resize(960 * 2);
        for (int i = 0; i < 960; ++i) {
            const int16_t sample = tone ? static_cast<int16_t>(12000 * std::sin(
                i * 6.283185307179586 * frequencies[(audioTick - 50) / 100] / 48000)) : 0;
            frame.samples[i * 2] = sample; frame.samples[i * 2 + 1] = sample;
        }
        publisher.pushAudio(frame);
        ++audioTick;
    });
    videoTimer.start(33);
    audioTimer.start(20);
    if (viewerStartMs > 0) QTimer::singleShot(viewerStartMs, &app, connectViewer);
    if (reconnectAtMs > 0) QTimer::singleShot(reconnectAtMs, &app, connectViewer);
    QTimer::singleShot(warmupMs + 17000, &app, [&] { app.quit(); });
    app.exec();
    videoTimer.stop(); audioTimer.stop();
    viewer.disconnect(); publisher.disconnect(); other.disconnect();
    // Reject the old behaviour which subscribed to the unwanted track and
    // only then ignored its callback, rather than checking rendered frames alone.
    QFile logFile(logPath);
    bool subscriptionOk = logFile.open(QIODevice::ReadOnly) && logFile.seek(logStart);
    bool receivedStats = false;
    if (subscriptionOk) {
        for (const auto& line : logFile.readAll().split('\n')) {
            if (line.contains("LiveKit track ignored:")) subscriptionOk = false;
            if (line.contains("rtc rx ")) {
                receivedStats = true;
                if (!line.contains("size=1920x1080")) subscriptionOk = false;
            }
        }
    }
    subscriptionOk = subscriptionOk && receivedStats;
    auto percentile = [](std::vector<double> values, double p) {
        if (values.empty()) return -1.0;
        std::sort(values.begin(), values.end());
        return values[static_cast<size_t>((values.size() - 1) * p)];
    };
    std::vector<double> audioDelays;
    for (size_t i = 0; i < audioSent.size(); ++i) {
        if (audioSent[i] && audioReceived[i]) audioDelays.push_back(audioReceived[i] - audioSent[i]);
    }
    const int seen = static_cast<int>(std::count(scenes.begin(), scenes.end(), true));
    std::printf("scenes=%d/6 frames=%zu invalid=%d video_delay_median_ms=%.1f p95_ms=%.1f max_gap_ms=%.1f "
                "audio_pulses=%zu/%zu audio_delay_median_ms=%.1f\n", seen, videoDelays.size(), invalid.load(),
        percentile(videoDelays, .5), percentile(videoDelays, .95), maxGap,
        audioDelays.size(), audioSent.size(), percentile(audioDelays, .5));
    std::printf("selected_subscription=%s\n", subscriptionOk ? "PASS" : "FAIL");
    const double videoMedian = percentile(videoDelays, .5);
    const double audioMedian = percentile(audioDelays, .5);
    const bool synchronized = expectedDelayMs <= 0 ||
        (videoMedian >= expectedDelayMs - 150 && audioMedian >= expectedDelayMs - 150 &&
         videoMedian < expectedDelayMs + 500 && audioMedian < expectedDelayMs + 500 &&
         std::abs(videoMedian - audioMedian) < 150);
    std::printf("buffered_av_sync=%s expected_ms=%d offset_ms=%.1f\n",
                synchronized ? "PASS" : "FAIL", expectedDelayMs, audioMedian - videoMedian);
    std::printf("steady_max_gap_ms=%.1f (excludes initial fill and reconnect fill)\n", steadyGap);
    const bool lifecycle = viewerStartMs > 0 || reconnectAtMs > 0;
    return seen >= (lifecycle ? 3 : 6) && videoDelays.size() > (lifecycle ? 100 : 150) &&
        invalid.load() == 0 && audioDelays.size() >= (lifecycle ? 2 : 4) && subscriptionOk && synchronized && connectionOk ? 0 : 5;
}
