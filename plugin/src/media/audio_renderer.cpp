#include "audio_renderer.h"
#include "playback_gain.h"
#include "audio_playout_queue.h"

#include "../log.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>

namespace ts3stream::media {
namespace {

constexpr int kOutputRate = 48000;
constexpr int kOutputChannels = 2;
constexpr int kBufferFrames = 960; // 20 ms
constexpr int kBufferCount = 4;

} // namespace

struct AudioRenderer::Impl {
    HWAVEOUT device = nullptr;
    WAVEFORMATEX format{};
    HANDLE doneEvent = nullptr;
    std::vector<std::vector<int16_t>> buffers;
    WAVEHDR headers[kBufferCount]{};

    std::thread thread;
    std::atomic<bool> running{false};
    std::atomic<int> volumePercent{100};

    std::mutex mutex;
    AudioPlayoutQueue queue;
    uint64_t submittedFrames = 0;
    uint64_t outputErrors = 0;

    ~Impl() {
        stop();
    }

    void stop() {
        running.store(false);
        if (doneEvent) SetEvent(doneEvent);
        if (thread.joinable()) {
            thread.join();
        }
        if (device) {
            // No concurrent waveOutWrite after resetting the device.
            waveOutReset(device);
            for (int i = 0; i < kBufferCount; ++i) {
                if (headers[i].dwFlags & WHDR_PREPARED) {
                    waveOutUnprepareHeader(device, &headers[i], sizeof(WAVEHDR));
                }
            }
            waveOutClose(device);
            device = nullptr;
        }
        if (doneEvent) {
            CloseHandle(doneEvent);
            doneEvent = nullptr;
        }
        buffers.clear();
        std::memset(headers, 0, sizeof(headers));
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.clear();
            submittedFrames = 0;
            outputErrors = 0;
        }
    }

    void fillBuffer(int index) {
        auto& buffer = buffers[index];
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.read(buffer.data(), kBufferFrames, AudioPlayoutQueue::Clock::now());
        }
        const int gain = volumePercent.load();
        for (auto& sample : buffer) sample = applyPlaybackGain(sample, gain);
    }

    bool submit(int index) {
        const auto result = waveOutWrite(device, &headers[index], sizeof(WAVEHDR));
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (result == MMSYSERR_NOERROR) submittedFrames += kBufferFrames;
            else ++outputErrors;
        }
        if (result == MMSYSERR_NOERROR) return true;
        diag::log("AudioRenderer waveOutWrite failed: %u", result);
        running.store(false);
        return false;
    }

    void run() {
        // Prime all buffers so the device always has data.
        for (int i = 0; i < kBufferCount; ++i) {
            fillBuffer(i);
            headers[i].lpData = reinterpret_cast<LPSTR>(buffers[i].data());
            headers[i].dwBufferLength = static_cast<DWORD>(buffers[i].size() * sizeof(int16_t));
            const auto result = waveOutPrepareHeader(device, &headers[i], sizeof(WAVEHDR));
            if (result != MMSYSERR_NOERROR) {
                { std::lock_guard<std::mutex> lock(mutex); ++outputErrors; }
                diag::log("AudioRenderer waveOutPrepareHeader failed: %u", result);
                running.store(false);
                return;
            }
            if (!submit(i)) return;
        }

        AudioBufferOrder order;
        auto nextLog = AudioPlayoutQueue::Clock::now() + std::chrono::seconds(2);
        uint64_t lastDropped = 0;
        while (running.load()) {
            const DWORD wait = WaitForSingleObject(doneEvent, 100);
            if (!running.load()) {
                break;
            }
            if (wait != WAIT_OBJECT_0) {
                continue;
            }

            order.refill(kBufferCount, [&](int i) {
                return running.load() && (headers[i].dwFlags & WHDR_DONE) != 0;
            }, [&](int i) {
                fillBuffer(i);
                return submit(i);
            });
            const auto now = AudioPlayoutQueue::Clock::now();
            if (now >= nextLog) {
                size_t queued;
                double age;
                uint64_t dropped;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    queued = queue.queuedFrames(); age = queue.oldestMs(now); dropped = queue.droppedFrames();
                }
                diag::log("audio playout queue_ms=%.1f oldest_ms=%.1f dropped_ms=%.1f device_capacity_ms=80",
                    queued / 48.0, age, (dropped - lastDropped) / 48.0);
                lastDropped = dropped;
                nextLog = now + std::chrono::seconds(2);
            }
        }
    }
};

AudioRenderer::AudioRenderer() : impl_(std::make_unique<Impl>()) {}

AudioRenderer::~AudioRenderer() = default;

bool AudioRenderer::start(int sampleRate, int channels) {
    stop();
    if (sampleRate != kOutputRate) {
        diag::log("AudioRenderer unsupported sample rate: %d", sampleRate);
        return false;
    }
    if (channels != 1 && channels != 2) {
        diag::log("AudioRenderer unsupported channels: %d", channels);
        return false;
    }

    impl_->format.wFormatTag = WAVE_FORMAT_PCM;
    impl_->format.nChannels = kOutputChannels;
    impl_->format.nSamplesPerSec = kOutputRate;
    impl_->format.wBitsPerSample = 16;
    impl_->format.nBlockAlign = kOutputChannels * 2;
    impl_->format.nAvgBytesPerSec = kOutputRate * impl_->format.nBlockAlign;
    impl_->format.cbSize = 0;

    impl_->doneEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!impl_->doneEvent) {
        return false;
    }

    MMRESULT result = waveOutOpen(&impl_->device, WAVE_MAPPER, &impl_->format,
                                  reinterpret_cast<DWORD_PTR>(impl_->doneEvent), 0, CALLBACK_EVENT);
    if (result != MMSYSERR_NOERROR) {
        diag::log("waveOutOpen failed: %u", result);
        CloseHandle(impl_->doneEvent);
        impl_->doneEvent = nullptr;
        return false;
    }

    impl_->buffers.resize(kBufferCount);
    for (auto& buffer : impl_->buffers) {
        buffer.assign(static_cast<size_t>(kBufferFrames) * kOutputChannels, 0);
    }

    impl_->running.store(true);
    impl_->thread = std::thread([impl = impl_.get()] { impl->run(); });
    diag::log("AudioRenderer started: 48000 Hz stereo");
    return true;
}

void AudioRenderer::stop() {
    impl_->stop();
}

void AudioRenderer::setVolume(int percent) {
    impl_->volumePercent.store(std::clamp(percent, 0, kMaxPlaybackVolumePercent));
}

void AudioRenderer::push(const int16_t* samples, int frames, int channels) {
    if (!samples || frames <= 0 || (channels != 1 && channels != 2)) {
        return;
    }

    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!impl_->running.load()) return;
        impl_->queue.push(samples, frames, channels, AudioPlayoutQueue::Clock::now());
    }
}

AudioRenderer::Stats AudioRenderer::stats() const {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return {impl_->queue.queuedFrames(), impl_->queue.droppedFrames(), impl_->submittedFrames, impl_->outputErrors};
}

} // namespace ts3stream::media
