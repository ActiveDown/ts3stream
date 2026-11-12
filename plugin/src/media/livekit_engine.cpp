#include "livekit_engine.h"
#include "video_send_queue.h"
#include "video_bitrate.h"
#include "rtc_diagnostics.h"
#include "delayed_audio.h"

#include "../log.h"

#include "livekit/livekit.h"

#include <windows.h>

#include <atomic>
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <future>
#include <limits>
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

        void onTrackPublished(livekit::Room&, const livekit::TrackPublishedEvent& event) override {
            if (event.participant) owner->subscribeSelectedParticipant(*event.participant);
        }

        void onTrackSubscribed(livekit::Room& room, const livekit::TrackSubscribedEvent& event) override {
            if (!event.track || !event.participant || !event.publication) {
                return;
            }

            const std::string identity = event.participant->identity();
            if (!owner->targetIdentity.empty() && identity != owner->targetIdentity) {
                event.publication->setSubscribed(false);
                diag::log("LiveKit track ignored: participant=%s (target=%s)",
                          identity.c_str(), owner->targetIdentity.c_str());
                return;
            }
            const std::string trackName = event.publication->name();
            diag::log("LiveKit track subscribed: participant=%s track=%s kind=%d",
                      identity.c_str(), trackName.c_str(), static_cast<int>(event.track->kind()));

            if (event.track->kind() == livekit::TrackKind::KIND_VIDEO) {
                owner->configureAudioDelay(room);
                owner->beginVideoBuffering();
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
                owner->configureAudioDelay(room);
                owner->delayedAudio.setAccepting(true);
                livekit::AudioStream::Options options;
                options.capacity = 8;
                room.setOnAudioFrameCallback(identity, trackName,
                    [owner = owner](const livekit::AudioFrame& frame) {
                        owner->handleAudioFrame(frame);
                    },
                    options);
            }
        }

        void onParticipantDisconnected(livekit::Room&, const livekit::ParticipantDisconnectedEvent& event) override {
            if (event.participant) owner->handleStreamEnded(event.participant->identity());
        }

        void onReconnecting(livekit::Room&, const livekit::ReconnectingEvent&) override {
            owner->delayedAudio.setAccepting(false);
        }

        void onDisconnected(livekit::Room&, const livekit::DisconnectedEvent&) override {
            owner->delayedAudio.setAccepting(false);
        }

        void onReconnected(livekit::Room&, const livekit::ReconnectedEvent&) override {
            owner->delayedAudio.setAccepting(true);
            owner->beginVideoBuffering();
        }

        void onTrackUnsubscribed(livekit::Room&, const livekit::TrackUnsubscribedEvent& event) override {
            if (event.track && event.track->kind() == livekit::TrackKind::KIND_AUDIO && event.participant &&
                (owner->targetIdentity.empty() || owner->targetIdentity == event.participant->identity())) {
                owner->delayedAudio.setAccepting(false);
            }
        }

        void onTrackUnpublished(livekit::Room&, const livekit::TrackUnpublishedEvent& event) override {
            if (event.participant && event.publication &&
                event.publication->kind() == livekit::TrackKind::KIND_VIDEO) {
                owner->handleStreamEnded(event.participant->identity());
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
    bool selectiveSubscription = false;
    std::atomic<int> videoWidth{0};
    std::atomic<int> videoHeight{0};
    std::atomic<int> videoFps{30};
    int videoBitrate = 0; // protected by roomMutex, like publication settings
    std::atomic<bool> videoStarted{false};
    std::atomic<bool> audioStarted{false};
    std::atomic<bool> videoErrorLogged{false};
    std::atomic<bool> audioErrorLogged{false};
    std::atomic<bool> firstVideoFrameLogged{false};
    std::atomic<bool> firstAudioFrameLogged{false};

    std::thread videoSender;
    VideoSendQueue videoQueue;
    std::thread statsThread;
    std::mutex statsMutex;
    std::condition_variable statsCv;
    bool statsStop = false;
    DelayedAudio delayedAudio;
    std::mutex audioDelayMutex;
    std::atomic<int> audioDelayMs{-1};
    std::atomic<int> serverDelayMs{0};
    std::atomic<bool> audioReceiving{false};
    std::atomic<int64_t> videoNotBeforeUs{0};

    VideoFrameCallback videoCallback;
    AudioFrameCallback audioCallback;
    StreamEndedCallback streamEndedCallback;

    void configureAudioDelay(livekit::Room& connectedRoom) {
        std::lock_guard<std::mutex> lock(audioDelayMutex);
        if (!audioReceiving.load() || audioDelayMs.load() >= 0) return;
        const int delay = roomAudioDelayMs(connectedRoom.roomInfo().metadata);
        const int serverDelay = roomServerDelayMs(connectedRoom.roomInfo().metadata);
        serverDelayMs.store(serverDelay);
        if (serverDelay > 0) diag::log("LiveKit server A/V buffer=%d ms; client PCM compensation=0 ms", serverDelay);
        if (delay > 0) {
            delayedAudio.start(delay, [this](const int16_t* pcm, int frames, int channels, int rate) {
                deliverAudio(pcm, frames, channels, rate);
            });
        }
        audioDelayMs.store(delay);
        diag::log("LiveKit room audio compensation=%d ms (actual room metadata)", delay);
    }

    void stopAudioDelay() {
        audioReceiving.store(false);
        std::lock_guard<std::mutex> lock(audioDelayMutex);
        delayedAudio.stop();
    }

    void beginVideoBuffering() {
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        // Suppress WebRTC's initial low-delay frames while it is filling the
        // requested jitter buffer, rather than showing a brief unsynced preview.
        videoNotBeforeUs.store(now + std::max(0, audioDelayMs.load()) * 1000LL);
    }

    void subscribeSelectedParticipant(livekit::RemoteParticipant& participant) {
        if (!selectiveSubscription || participant.identity() != targetIdentity) return;
        // SDK 1.13.1 may leave TrackPublishedEvent::publication empty after
        // moving it into this map. Resolve publications from the participant.
        for (const auto& entry : participant.trackPublications()) {
            const auto& publication = entry.second;
            if (publication && !publication->subscribed()) {
                publication->setSubscribed(true);
                diag::log("LiveKit selected track subscription requested: kind=%d", static_cast<int>(publication->kind()));
            }
        }
    }

    void handleStreamEnded(const std::string& identity) {
        if (targetIdentity.empty() || identity != targetIdentity) return;
        delayedAudio.setAccepting(false);
        StreamEndedCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            callback = std::move(streamEndedCallback);
        }
        if (callback) {
            diag::log("LiveKit target stream ended: %s", identity.c_str());
            callback();
        }
    }

    ~Impl() {
        std::lock_guard<std::mutex> lock(roomMutex);
        disconnectLocked();
    }

    void stopStats() {
        {
            std::lock_guard<std::mutex> lock(statsMutex);
            statsStop = true;
        }
        statsCv.notify_all();
        if (statsThread.joinable()) statsThread.join();
    }

    void startStats() {
        stopStats();
        statsStop = false;
        statsThread = std::thread([this] {
            RtcDiagnostics diagnostics;
            std::future<livekit::SessionStats> pending;
            while (true) {
                {
                    std::unique_lock<std::mutex> lock(statsMutex);
                    if (statsCv.wait_for(lock, std::chrono::seconds(2), [this] { return statsStop; })) break;
                }
                try {
                    // Never block capture/UI or accumulate unresolved requests.
                    // Room remains alive until stopStats has joined this thread.
                    if (!pending.valid()) pending = room->getStats();
                    if (pending.wait_for(std::chrono::milliseconds(100)) != std::future_status::ready) continue;
                    const auto stats = pending.get();
                    const double now = std::chrono::duration<double>(
                        std::chrono::steady_clock::now().time_since_epoch()).count();
                    for (const auto& line : diagnostics.update(stats, now)) diag::log("%s", line.c_str());
                } catch (const std::exception& e) {
                    logError("stats", e);
                }
            }
        });
    }

    void stopVideoSenderLocked() {
        videoQueue.stop();
        if (videoSender.joinable()) {
            videoSender.join();
        }
        videoQueue.reset();
    }

    void disconnectLocked() {
        stopStats();
        stopAudioDelay();
        {
            std::lock_guard<std::mutex> callbackLock(callbackMutex);
            streamEndedCallback = nullptr;
        }
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
        audioDelayMs.store(-1);
        serverDelayMs.store(0);
        videoNotBeforeUs.store(0);

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
        const auto now = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        if (now < videoNotBeforeUs.load()) return;
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
        if (!audioReceiving.load() || frame.data().empty()) {
            return;
        }

        if (!firstAudioFrameLogged.exchange(true)) {
            diag::log("LiveKit first received audio frame: rate=%d channels=%d frames=%d",
                      frame.sampleRate(), frame.numChannels(), frame.samplesPerChannel());
        }
        if (audioDelayMs.load() > 0) {
            delayedAudio.push(frame.data().data(), frame.samplesPerChannel(), frame.numChannels(), frame.sampleRate());
        } else {
            deliverAudio(frame.data().data(), frame.samplesPerChannel(), frame.numChannels(), frame.sampleRate());
        }
    }

    void deliverAudio(const int16_t* pcm, int frames, int channels, int rate) {
        AudioFrameCallback callback;
        {
            std::lock_guard<std::mutex> lock(callbackMutex);
            callback = audioCallback;
        }
        if (callback) callback(pcm, frames, channels, rate);
    }

    void videoSenderLoop(std::shared_ptr<livekit::VideoSource> source, int width, int height) {
        int sent = 0;
        std::unique_ptr<livekit::VideoFrame> latestFrame;
        int64_t lastTimestampUs = 0;
        while (!videoQueue.stopped()) {
            auto frame = videoQueue.take();
            if (videoQueue.stopped()) break;

            if (!source) {
                continue;
            }
            try {
                if (frame) {
                    const int rowBytes = frame->width * 4;
                    std::vector<uint8_t> pixels(static_cast<size_t>(rowBytes) * frame->height);
                    for (int y = 0; y < frame->height; ++y) {
                        std::memcpy(pixels.data() + static_cast<size_t>(y) * rowBytes,
                                    frame->bgra.data() + static_cast<size_t>(y) * frame->stride,
                                    rowBytes);
                    }
                    latestFrame = std::make_unique<livekit::VideoFrame>(frame->width, frame->height,
                        livekit::VideoBufferType::BGRA, std::move(pixels));
                    // Keep the same published track and source. WebRTC accepts
                    // per-frame dimensions; republishing would close viewers.
                    if (width != frame->width || height != frame->height) {
                        diag::log("LiveKit video size changed: %dx%d -> %dx%d", width, height,
                                  frame->width, frame->height);
                        width = frame->width;
                        height = frame->height;
                    }
                }
                if (!latestFrame) continue;
                const int64_t nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now().time_since_epoch()).count();
                lastTimestampUs = std::max(nowUs, lastTimestampUs + 1);
                videoQueue.markSent(videoFps.load());
                source->captureFrame(*latestFrame, lastTimestampUs,
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
    impl_->stopStats();
    impl_->stopAudioDelay();
    impl_->videoStarted.store(false);
    impl_->audioStarted.store(false);
    impl_->stopVideoSenderLocked();
    {
        std::lock_guard<std::mutex> callbackLock(impl_->callbackMutex);
        impl_->videoCallback = nullptr;
        impl_->audioCallback = nullptr;
        impl_->streamEndedCallback = nullptr;
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
    StreamEndedCallback savedStreamEndedCallback;
    {
        std::lock_guard<std::mutex> callbackLock(impl_->callbackMutex);
        savedVideoCallback = impl_->videoCallback;
        savedAudioCallback = impl_->audioCallback;
        savedStreamEndedCallback = impl_->streamEndedCallback;
    }

    impl_->disconnectLocked();

    if (savedVideoCallback || savedAudioCallback || savedStreamEndedCallback) {
        std::lock_guard<std::mutex> callbackLock(impl_->callbackMutex);
        impl_->videoCallback = std::move(savedVideoCallback);
        impl_->audioCallback = std::move(savedAudioCallback);
        impl_->streamEndedCallback = std::move(savedStreamEndedCallback);
    }

    impl_->targetIdentity = targetIdentity;
    impl_->selectiveSubscription = autoSubscribe && !targetIdentity.empty();
    impl_->audioReceiving.store(true);
    if (!targetIdentity.empty()) {
        diag::log("LiveKit connect target participant=%s", targetIdentity.c_str());
    }

    try {
        auto room = std::make_unique<livekit::Room>();
        auto delegate = std::make_unique<Impl::Delegate>(impl_.get());
        room->setDelegate(delegate.get());

        livekit::RoomOptions options;
        // Ignoring callbacks is too late: auto-subscription already downloads
        // every publisher in the TS3 channel. Request only the selected author.
        options.auto_subscribe = autoSubscribe && !impl_->selectiveSubscription;
        options.adaptive_stream = false;
        options.dynacast = false;
        options.single_peer_connection = true;

        if (!room->connect(url, token, options)) {
            error = "room connect failed";
            return false;
        }
        if (autoSubscribe) impl_->configureAudioDelay(*room);
        if (impl_->selectiveSubscription) {
            if (auto participant = room->remoteParticipant(targetIdentity).lock()) {
                impl_->subscribeSelectedParticipant(*participant);
            }
        }

        impl_->delegate = std::move(delegate);
        impl_->room = std::move(room);
        impl_->startStats();
        diag::log("LiveKit connected to %s (autoSubscribe=%d)", url.c_str(), autoSubscribe ? 1 : 0);
        return true;
    } catch (const std::exception& e) {
        logError("connect", e);
        error = e.what();
        impl_->stopStats();
        impl_->stopAudioDelay();
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

int LiveKitEngine::playbackDelayMs() const {
    return std::max(0, impl_->audioDelayMs.load());
}

int LiveKitEngine::serverBufferDelayMs() const {
    return impl_->serverDelayMs.load();
}

void LiveKitEngine::setVideoFrameCallback(VideoFrameCallback callback) {
    std::lock_guard<std::mutex> lock(impl_->callbackMutex);
    impl_->videoCallback = std::move(callback);
}

void LiveKitEngine::setAudioFrameCallback(AudioFrameCallback callback) {
    std::lock_guard<std::mutex> lock(impl_->callbackMutex);
    impl_->audioCallback = std::move(callback);
}

void LiveKitEngine::setStreamEndedCallback(StreamEndedCallback callback) {
    std::lock_guard<std::mutex> lock(impl_->callbackMutex);
    impl_->streamEndedCallback = std::move(callback);
}

bool LiveKitEngine::ensureVideo(int width, int height, int fps, std::string& error, int maxBitrateKbps) {
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
    const int maxBitrate = videoBitrateBps(maxBitrateKbps, std::getenv("TS3STREAM_MAX_BITRATE"));
    if (impl_->videoStarted.load() && impl_->videoWidth.load() == width &&
        impl_->videoHeight.load() == height && impl_->videoFps.load() == fps && impl_->videoBitrate == maxBitrate) {
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
        // A busy scene should reduce resolution before sacrificing motion.
        // The WebRTC congestion controller/pacer still owns the network rate;
        // buffering raw frames here would not smooth encoded bitrate spikes.
        options.degradation_preference = livekit::DegradationPreference::MaintainFramerate;
        // WebRTC regulates the actual send rate from REMB/TWCC feedback. This
        // value is the ceiling, not a fixed rate: on a clean channel the
        // encoder climbs towards it, and congestion control lowers it
        // automatically when loss/RTT grows.
        options.video_encoding = livekit::VideoEncodingOptions{
            static_cast<uint64_t>(maxBitrate), static_cast<double>(fps)};
        options.video_encoder = livekit::VideoEncoderBackend::Auto;
        options.stream = "screen";

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
        impl_->videoBitrate = maxBitrate;
        impl_->videoStarted.store(true);
        impl_->videoSender = std::thread([impl = impl_.get(), source, width, height] {
            impl->videoSenderLoop(source, width, height);
        });
        diag::log("LiveKit video track published: %dx%d fps=%d maxBitrate=%d", width, height, fps, maxBitrate);
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
        // Group related A/V tracks. This alone does NOT synchronize raw audio
        // callbacks with a server-requested video playout delay (see benchmark).
        options.stream = "screen";
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
    if (frame.width <= 0 || frame.height <= 0 || frame.width > std::numeric_limits<int>::max() / 4 ||
        frame.stride < frame.width * 4 ||
        frame.bgra.size() / static_cast<size_t>(frame.stride) < static_cast<size_t>(frame.height)) {
        if (!impl_->videoErrorLogged.exchange(true)) {
            diag::log("LiveKit invalid video frame: %dx%d stride=%d", frame.width, frame.height, frame.stride);
        }
        return;
    }

    impl_->videoQueue.push(std::make_shared<CapturedVideoFrame>(std::move(frame)));
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
