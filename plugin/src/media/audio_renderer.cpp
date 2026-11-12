#include "audio_renderer.h"
#include "playback_gain.h"

#include "../log.h"

#include <windows.h>
#include <mmsystem.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

namespace ts3stream::media {
namespace {

constexpr int kOutputRate = 48000;
constexpr int kOutputChannels = 2;
constexpr int kBufferFrames = 960; // 20 ms
constexpr int kBufferCount = 4;
constexpr size_t kMaxQueuedSamples = static_cast<size_t>(kOutputRate) * kOutputChannels * 2;

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
    std::condition_variable cv;
    std::deque<int16_t> queue;

    ~Impl() {
        stop();
    }

    void stop() {
        running.store(false);
        cv.notify_all();
        if (device) {
            waveOutReset(device);
        }
        if (thread.joinable()) {
            thread.join();
        }
        if (device) {
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
        {
            std::lock_guard<std::mutex> lock(mutex);
            queue.clear();
        }
    }

    void fillBuffer(int index) {
        auto& buffer = buffers[index];
        std::fill(buffer.begin(), buffer.end(), 0);

        std::lock_guard<std::mutex> lock(mutex);
        const size_t wanted = static_cast<size_t>(kBufferFrames) * kOutputChannels;
        size_t written = 0;
        const int gain = volumePercent.load();
        while (written < wanted && !queue.empty()) {
            buffer[written++] = applyPlaybackGain(queue.front(), gain);
            queue.pop_front();
        }
    }

    void run() {
        // Prime all buffers so the device always has data.
        for (int i = 0; i < kBufferCount; ++i) {
            fillBuffer(i);
            headers[i].lpData = reinterpret_cast<LPSTR>(buffers[i].data());
            headers[i].dwBufferLength = static_cast<DWORD>(buffers[i].size() * sizeof(int16_t));
            waveOutPrepareHeader(device, &headers[i], sizeof(WAVEHDR));
            waveOutWrite(device, &headers[i], sizeof(WAVEHDR));
        }

        while (running.load()) {
            const DWORD wait = WaitForSingleObject(doneEvent, 100);
            if (!running.load()) {
                break;
            }
            if (wait != WAIT_OBJECT_0) {
                continue;
            }

            for (int i = 0; i < kBufferCount; ++i) {
                if ((headers[i].dwFlags & WHDR_DONE) == 0) {
                    continue;
                }
                headers[i].dwFlags &= ~WHDR_DONE;
                fillBuffer(i);
                headers[i].lpData = reinterpret_cast<LPSTR>(buffers[i].data());
                headers[i].dwBufferLength = static_cast<DWORD>(buffers[i].size() * sizeof(int16_t));
                waveOutWrite(device, &headers[i], sizeof(WAVEHDR));
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
        for (int i = 0; i < frames; ++i) {
            if (channels == 1) {
                impl_->queue.push_back(samples[i]);
                impl_->queue.push_back(samples[i]);
            } else {
                impl_->queue.push_back(samples[i * 2]);
                impl_->queue.push_back(samples[i * 2 + 1]);
            }
        }
        while (impl_->queue.size() > kMaxQueuedSamples) {
            impl_->queue.pop_front();
        }
    }
    impl_->cv.notify_one();
}

} // namespace ts3stream::media
