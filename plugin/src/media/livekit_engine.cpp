#include "livekit_engine.h"

#include "../log.h"

#include "livekit/livekit.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

namespace ts3stream::media {
namespace {

std::mutex g_sdkMutex;
bool g_sdkInitialized = false;

void logError(const char* what, const std::exception& error) {
    diag::log("LiveKit %s: %s", what, error.what());
}

} // namespace

struct LiveKitEngine::Impl {
    struct Delegate : livekit::RoomDelegate {
        explicit Delegate(Impl* owner) : owner(owner) {}

        void onTrackSubscribed(livekit::Room& room, const livekit::TrackSubscribedEvent& event) override {
            if (!event.track || !event.participant || !event.publication) {
                return;
            }

            const std::string identity = event.participant->identity();
            if (!owner->targetIdentity.empty() && identity != owner->targetIdentity) {
                diag::log("LiveKit track ignored: participant=%s (target=%s)",
                          identity.c_str(), owner->targetIdentity.c_str());
                return;
            }
            const std::string trackName = event.publication->name();
            diag::log("LiveKit track subscribed: participant=%s track=%s kind=%d",
                      identity.c_str(), trackName.c_str(), static_cast<int>(event.track->kind()));

            if (event.track->kind() == livekit::TrackKind::KIND_VIDEO) {
                try {
                    livekit::VideoStream::Options options;
                    options.capacity = 2;
                    options.format = livekit::VideoBufferType::RGBA;
                    room.setOnVideoFrameCallback(identity, trackName,
                        [owner = owner](const livekit::VideoFrame& frame, int64_t timestampUs) {
                            owner->handleVideoFrame(frame, timestampUs);
                        },
                        options);
                    diag::log("LiveKit video callback registered for %s/%s", identity.c_str(), trackName.c_str());
                } catch (const std::exception& e) {
                    diag::log("LiveKit setOnVideoFrameCallback failed: %s", e.what());
                }
            } else if (event.track->kind() == livekit::TrackKind::KIND_AUDIO) {
                livekit::AudioStream::Options options;
                options.capacity = 8;
                room.setOnAudioFrameCallback(identity, trackName,
                    [owner = owner](const livekit::AudioFrame& frame) {
                        owner->handleAudioFrame(frame);
                    },
                    options);
            }
        }

        Impl* owner;
    };

    mutable std::mutex roomMutex;
    mutable std::mutex callbackMutex;

    std::unique_ptr<livekit::Room> room;
    std::unique_ptr<Delegate> delegate;

    std::shared_ptr<livekit::VideoSource> videoSource;
    std::shared_ptr<livekit::AudioSource> audioSource;
    std::string targetIdentity;
    std::atomic<int> videoWidth{0};
    std::atomic<int> videoHeight{0};
    std::atomic<int> videoFps{30};
    std::atomic<bool> videoStarted{false};
    std::atomic<bool> audioStarted{false};
    std::atomic<bool> videoSenderStop{false};
    std::atomic<bool> videoErrorLogged{false};
    std::atomic<bool> audioErrorLogged{false};
    std::atomic<bool> firstVideoFrameLogged{false};
    std::atomic<bool> firstAudioFrameLogged{false};

    std::thread videoSender;
    std::mutex videoQueueMutex;
    std::condition_variable videoQueueCv;
    std::shared_ptr<CapturedVideoFrame> pendingVideo;

    VideoFrameCallback videoCallback;
    AudioFrameCallback audioCallback;

    ~Impl() {
        std::lock_guard<std::mutex> lock(roomMutex);
        disconnectLocked();
    }

    void stopVideoSenderLocked() {
        videoSenderStop.store(true);
        videoQueueCv.notify_all();
        if (videoSender.joinable()) {
            videoSender.join();
        }
        {
            std::lock_guard<std::mutex> queueLock(videoQueueMutex);
            pendingVideo.reset();
        }
        videoSenderStop.store(false);
    }

    void disconnectLocked() {
        videoStarted.store(false);
        audioStarted.store(false);

        stopVideoSenderLocked();

        if (room) {
            try {
                room->disconnect();
            } catch (const std::exception& e) {
                logError("disconnect", e);
            }
            room.reset();
        }
        delegate.reset();

        videoSource.reset();
        audioSource.reset();
        targetIdentity.clear();

        {
            std::lock_guard<std::mutex> callbackLock(callbackMutex);
            videoCallback = nullptr;
            audioCallback = nullptr;
        }

        videoWidth.store(0);
        videoHeight.store(0);
        videoErrorLogged.store(false);
        audioErrorLogged.store(false);
        firstVideoFrameLogged.store(false);
        firstAudioFrameLogged.store(false);
    }

    void handleVideoFrame(const livekit::VideoFrame& frame, int64_t timestampUs) {
        VideoFrameCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            callback = videoCallback;
        }
        if (!callback || frame.width() <= 0 || frame.height() <= 0 || !frame.data()) {
            return;
        }

        if (!firstVideoFrameLogged.exchange(true)) {
            diag::log("LiveKit first received video frame: %dx%d", frame.width(), frame.height());
        }
        callback(frame.width(), frame.height(), frame.data(), frame.width() * 4, timestampUs);
    }

    void handleAudioFrame(const livekit::AudioFrame& frame) {
        AudioFrameCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            callback = audioCallback;
        }
        if (!callback || frame.data().empty()) {
            return;
        }

        if (!firstAudioFrameLogged.exchange(true)) {
            diag::log("LiveKit first received audio frame: rate=%d channels=%d frames=%d",
                      frame.sampleRate(), frame.numChannels(), frame.samplesPerChannel());
        }
        callback(frame.data().data(), frame.samplesPerChannel(), frame.numChannels(), frame.sampleRate());
    }

    void videoSenderLoop(std::shared_ptr<livekit::VideoSource> source, int width, int height) {
        int sent = 0;
        while (!videoSenderStop.load()) {
            std::shared_ptr<CapturedVideoFrame> frame;
            {
                std::unique_lock<std::mutex> lock(videoQueueMutex);
                videoQueueCv.wait(lock, [this] {
                    return videoSenderStop.load() || pendingVideo != nullptr;
                });
                if (videoSenderStop.load()) {
                    break;
                }
                frame = std::move(pendingVideo);
                pendingVideo.reset();
            }

            if (!frame || !source) {
                continue;
            }
            if (frame->width != width || frame->height != height) {
                continue;
            }

            try {
                const int rowBytes = frame->width * 4;
                std::vector<uint8_t> pixels(static_cast<size_t>(rowBytes) * frame->height);
                for (int y = 0; y < frame->height; ++y) {
                    std::memcpy(pixels.data() + static_cast<size_t>(y) * rowBytes,
                                frame->bgra.data() + static_cast<size_t>(y) * frame->stride,
                                rowBytes);
                }

                livekit::VideoFrame videoFrame(frame->width, frame->height,
                                               livekit::VideoBufferType::BGRA, std::move(pixels));
                source->captureFrame(videoFrame, frame->timestampUs,
                                     livekit::VideoRotation::VIDEO_ROTATION_0);

                if ((sent % 30) == 0) {
                    diag::log("LiveKit video sender: frame #%d %dx%d", sent, width, height);
                }
                ++sent;
            } catch (const std::exception& e) {
                if (!videoErrorLogged.exchange(true)) {
                    logError("capture video frame", e);
                }
            }
        }
        diag::log("LiveKit video sender stopped");
    }
};

LiveKitEngine::LiveKitEngine() : impl_(std::make_unique<Impl>()) {}

LiveKitEngine::~LiveKitEngine() {
    if (leakOnDestroy_) {
        (void)impl_.release();
    }
}

void LiveKitEngine::disconnectForShutdown() {
    std::lock_guard<std::mutex> lock(impl_->roomMutex);
    impl_->videoStarted.store(false);
    impl_->audioStarted.store(false);
    impl_->stopVideoSenderLocked();
    {
        std::lock_guard<std::mutex> callbackLock(impl_->callbackMutex);
        impl_->videoCallback = nullptr;
        impl_->audioCallback = nullptr;
    }
    leakOnDestroy_ = true;
    diag::log("LiveKit engine leaked for safe shutdown");
}

void LiveKitEngine::shutdownSdk() {
    std::lock_guard<std::mutex> sdkLock(g_sdkMutex);
    if (!g_sdkInitialized) {
        return;
    }
    g_sdkInitialized = false;
    try {
        livekit::shutdown();
        diag::log("LiveKit SDK shutdown");
    } catch (const std::exception& e) {
        logError("shutdown", e);
    } catch (...) {
        diag::log("LiveKit SDK shutdown threw an unknown exception");
    }
}

bool LiveKitEngine::connect(const std::string& url, const std::string& token, bool autoSubscribe,
                            std::string& error, const std::string& targetIdentity) {
    const std::wstring dllDir = diag::dataDir();
    SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_USER_DIRS);
    AddDllDirectory(dllDir.c_str());
    SetDllDirectoryW(dllDir.c_str());

    {
        std::lock_guard<std::mutex> sdkLock(g_sdkMutex);
        if (!g_sdkInitialized) {
            const bool initialized = livekit::initialize(livekit::LogLevel::Info);
            g_sdkInitialized = true;
            diag::log("LiveKit SDK initialize=%d", initialized ? 1 : 0);
        }
    }

    std::lock_guard<std::mutex> lock(impl_->roomMutex);

    // Callbacks are normally installed before connect(); disconnectLocked()
    // clears them, so preserve them across the reconnect.
    VideoFrameCallback savedVideoCallback;
    AudioFrameCallback savedAudioCallback;
    {
        std::lock_guard<std::mutex> callbackLock(impl_->callbackMutex);
        savedVideoCallback = impl_->videoCallback;
        savedAudioCallback = impl_->audioCallback;
    }

    impl_->disconnectLocked();

    if (savedVideoCallback || savedAudioCallback) {
        std::lock_guard<std::mutex> callbackLock(impl_->callbackMutex);
        impl_->videoCallback = std::move(savedVideoCallback);
        impl_->audioCallback = std::move(savedAudioCallback);
    }

    impl_->targetIdentity = targetIdentity;
    if (!targetIdentity.empty()) {
        diag::log("LiveKit connect target participant=%s", targetIdentity.c_str());
    }

    try {
        auto room = std::make_unique<livekit::Room>();
        auto delegate = std::make_unique<Impl::Delegate>(impl_.get());
        room->setDelegate(delegate.get());

        livekit::RoomOptions options;
        options.auto_subscribe = autoSubscribe;
        options.adaptive_stream = false;
        options.dynacast = false;
        options.single_peer_connection = true;

        if (!room->connect(url, token, options)) {
            error = "room connect failed";
            return false;
        }

        impl_->delegate = std::move(delegate);
        impl_->room = std::move(room);
        diag::log("LiveKit connected to %s (autoSubscribe=%d)", url.c_str(), autoSubscribe ? 1 : 0);
        return true;
    } catch (const std::exception& e) {
        logError("connect", e);
        error = e.what();
        impl_->room.reset();
        impl_->delegate.reset();
        return false;
    }
}

void LiveKitEngine::disconnect() {
    std::lock_guard<std::mutex> lock(impl_->roomMutex);
    impl_->disconnectLocked();
}

bool LiveKitEngine::isConnected() const {
    std::lock_guard<std::mutex> lock(impl_->roomMutex);
    return impl_->room != nullptr;
}

void LiveKitEngine::setVideoFrameCallback(VideoFrameCallback callback) {
    std::lock_guard<std::mutex> lock(impl_->callbackMutex);
    impl_->videoCallback = std::move(callback);
}

void LiveKitEngine::setAudioFrameCallback(AudioFrameCallback callback) {
    std::lock_guard<std::mutex> lock(impl_->callbackMutex);
    impl_->audioCallback = std::move(callback);
}

bool LiveKitEngine::ensureVideo(int width, int height, int fps, std::string& error) {
    std::lock_guard<std::mutex> lock(impl_->roomMutex);
    if (!impl_->room) {
        error = "not connected";
        return false;
    }
    if (width <= 0 || height <= 0) {
        error = "invalid video size";
        return false;
    }
    if (fps <= 0) {
        fps = 30;
    }
    if (impl_->videoStarted.load() && impl_->videoWidth.load() == width &&
        impl_->videoHeight.load() == height && impl_->videoFps.load() == fps) {
        return true;
    }

    impl_->stopVideoSenderLocked();
    impl_->videoSource.reset();
    impl_->videoStarted.store(false);

    try {
        auto source = std::make_shared<livekit::VideoSource>(width, height);
        auto track = livekit::LocalVideoTrack::createLocalVideoTrack("screen", source);

        livekit::TrackPublishOptions options;
        options.source = livekit::TrackSource::SOURCE_SCREENSHARE;
        options.video_codec = livekit::VideoCodec::H264;
        options.simulcast = false;
        // WebRTC regulates the actual send rate from REMB/TWCC feedback. This
        // value is the ceiling, not a fixed rate: on a clean channel the
        // encoder climbs towards it, and congestion control lowers it
        // automatically when loss/RTT grows.
        int maxBitrate = 12'000'000;
        if (const char* configured = std::getenv("TS3STREAM_MAX_BITRATE")) {
            const int parsed = std::atoi(configured);
            if (parsed >= 1'000'000) {
                maxBitrate = parsed;
            }
        }
        options.video_encoding = livekit::VideoEncodingOptions{
            static_cast<uint64_t>(maxBitrate), static_cast<double>(fps)};
        options.video_encoder = livekit::VideoEncoderBackend::Auto;

        auto participant = impl_->room->localParticipant().lock();
        if (!participant) {
            error = "local participant unavailable";
            return false;
        }
        participant->publishTrack(track, options);

        impl_->videoSource = source;
        impl_->videoWidth.store(width);
        impl_->videoHeight.store(height);
        impl_->videoFps.store(fps);
        impl_->videoStarted.store(true);
        impl_->videoSenderStop.store(false);
        impl_->videoSender = std::thread([impl = impl_.get(), source, width, height] {
            impl->videoSenderLoop(source, width, height);
        });
        diag::log("LiveKit video track published: %dx%d", width, height);
        return true;
    } catch (const std::exception& e) {
        logError("publish video", e);
        error = e.what();
        impl_->videoSource.reset();
        impl_->videoStarted.store(false);
        return false;
    }
}

bool LiveKitEngine::ensureAudio(std::string& error) {
    std::lock_guard<std::mutex> lock(impl_->roomMutex);
    if (!impl_->room) {
        error = "not connected";
        return false;
    }
    if (impl_->audioStarted.load()) {
        return true;
    }

    try {
        impl_->audioSource = std::make_shared<livekit::AudioSource>(48000, 2, 0);
        auto track = livekit::LocalAudioTrack::createLocalAudioTrack("screen-audio", impl_->audioSource);

        livekit::TrackPublishOptions options;
        options.source = livekit::TrackSource::SOURCE_SCREENSHARE_AUDIO;
        options.audio_encoding = livekit::AudioEncodingOptions{128'000};
        options.dtx = true;
        options.red = true;

        auto participant = impl_->room->localParticipant().lock();
        if (!participant) {
            error = "local participant unavailable";
            return false;
        }
        participant->publishTrack(track, options);

        impl_->audioStarted.store(true);
        diag::log("LiveKit audio track published");
        return true;
    } catch (const std::exception& e) {
        logError("publish audio", e);
        error = e.what();
        impl_->audioSource.reset();
        impl_->audioStarted.store(false);
        return false;
    }
}

void LiveKitEngine::pushVideo(CapturedVideoFrame frame) {
    if (!impl_->videoStarted.load()) {
        return;
    }
    if (frame.width != impl_->videoWidth.load() || frame.height != impl_->videoHeight.load()) {
        if (!impl_->videoErrorLogged.exchange(true)) {
            diag::log("LiveKit video size mismatch: track=%dx%d frame=%dx%d",
                      impl_->videoWidth.load(), impl_->videoHeight.load(), frame.width, frame.height);
        }
        return;
    }

    auto copy = std::make_shared<CapturedVideoFrame>(std::move(frame));
    {
        std::lock_guard<std::mutex> lock(impl_->videoQueueMutex);
        impl_->pendingVideo = std::move(copy);
    }
    impl_->videoQueueCv.notify_one();
}

void LiveKitEngine::pushAudio(const CapturedAudioFrame& frame) {
    std::shared_ptr<livekit::AudioSource> source;
    {
        std::lock_guard<std::mutex> lock(impl_->roomMutex);
        if (!impl_->audioStarted.load() || !impl_->audioSource) {
            return;
        }
        source = impl_->audioSource;
    }
    if (frame.sampleRate != 48000 || frame.channels != 2 || frame.samples.empty()) {
        return;
    }

    try {
        constexpr int kSamplesPerChannel = 480; // 10 ms at 48 kHz
        const size_t totalSamples = frame.samples.size();
        size_t offset = 0;
        while (offset + kSamplesPerChannel * 2 <= totalSamples) {
            std::vector<int16_t> chunk(frame.samples.begin() + offset,
                                       frame.samples.begin() + offset + kSamplesPerChannel * 2);
            livekit::AudioFrame audioFrame(std::move(chunk), 48000, 2, kSamplesPerChannel);
            source->captureFrame(audioFrame, 20);
            offset += kSamplesPerChannel * 2;
        }
    } catch (const std::exception& e) {
        if (!impl_->audioErrorLogged.exchange(true)) {
            logError("capture audio frame", e);
        }
    }
}

} // namespace ts3stream::media
