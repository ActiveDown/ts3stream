#pragma once

#include "wasapi_capture.h"
#include "wgc_capture.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace ts3stream::media {

class LiveKitEngine {
public:
    using VideoFrameCallback = std::function<void(int width, int height, const uint8_t* rgba, int stride, int64_t timestampUs)>;
    using AudioFrameCallback = std::function<void(const int16_t* samples, int frames, int channels, int sampleRate)>;
    using StreamEndedCallback = std::function<void()>;

    LiveKitEngine();
    ~LiveKitEngine();

    // Must be called once at process/plugin shutdown, after all engines are gone.
    static void shutdownSdk();

    LiveKitEngine(const LiveKitEngine&) = delete;
    LiveKitEngine& operator=(const LiveKitEngine&) = delete;

    bool connect(const std::string& url, const std::string& token, bool autoSubscribe,
                 std::string& error, const std::string& targetIdentity = {});
    void disconnect();
    // Deliberately leaks the SDK room during process/plugin shutdown. The
    // native LiveKit teardown path crashes inside TS3 at exit; the process is
    // terminating anyway, so the OS reclaims the handles.
    void disconnectForShutdown();

    bool isConnected() const;
    int playbackDelayMs() const;

    void setVideoFrameCallback(VideoFrameCallback callback);
    void setAudioFrameCallback(AudioFrameCallback callback);
    // Called on an SDK thread; the UI must queue teardown onto its own thread.
    void setStreamEndedCallback(StreamEndedCallback callback);

    bool ensureVideo(int width, int height, int fps, std::string& error);
    bool ensureAudio(std::string& error);

    void pushVideo(CapturedVideoFrame frame);
    void pushAudio(const CapturedAudioFrame& frame);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    bool leakOnDestroy_ = false;
};

} // namespace ts3stream::media
