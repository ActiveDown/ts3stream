#pragma once

#include <algorithm>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ts3stream::media {

// Control API sets this marker AND matching min/max WebRTC playout delay in
// RoomConfiguration. Read the actual room metadata, never the requested token
// configuration: an already-existing room can retain its old settings.
inline int roomDelayMs(const std::string& metadata, const std::string& prefix) {
    const size_t length = prefix.size();
    if (metadata.compare(0, length, prefix) != 0) return 0;
    int delay = 0;
    const auto result = std::from_chars(metadata.data() + length, metadata.data() + metadata.size(), delay);
    return result.ec == std::errc{} && result.ptr == metadata.data() + metadata.size() &&
        delay >= 0 && delay <= 5000 && delay % 10 == 0 ? delay : 0;
}

inline int roomAudioDelayMs(const std::string& metadata) {
    return roomDelayMs(metadata, "ts3stream-av-delay-v1:");
}

inline int roomServerDelayMs(const std::string& metadata) {
    return roomDelayMs(metadata, "ts3stream-server-av-v1:");
}

// Native WebRTC already timestamps/buffers compressed video. The C++ raw PCM
// sink bypasses its A/V playout synchronization. Compensate the room's extra
// video delay here, before waveOut, without retaining decoded video frames.
// Absolute monotonic deadlines prevent cumulative delay on a long session.
class DelayedAudio {
public:
    using Clock = std::chrono::steady_clock;
    using Handler = std::function<void(const int16_t*, int, int, int)>;
    ~DelayedAudio() { stop(); }

    void start(int delayMs, Handler handler) {
        stop();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            delay_ = std::chrono::milliseconds(std::clamp(delayMs, 0, 5000));
            maxSamples_ = static_cast<size_t>(48000 * 2) * (delay_.count() + 500) / 1000;
            handler_ = std::move(handler);
            stopped_ = false;
            accepting_ = true;
        }
        thread_ = std::thread([this] { run(); });
    }

    // Called from the owning/control thread, never from the output handler.
    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
            accepting_ = false;
            queue_.clear();
            samples_ = 0;
        }
        cv_.notify_all();
        if (thread_.joinable()) thread_.join();
        handler_ = nullptr;
    }

    void setAccepting(bool accepting) {
        std::lock_guard<std::mutex> outputLock(outputMutex_);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            ++generation_;
            accepting_ = accepting;
            queue_.clear();
            samples_ = 0;
        }
        cv_.notify_all();
    }

    void push(const int16_t* pcm, int frames, int channels, int sampleRate) {
        if (!pcm || sampleRate != 48000 || frames <= 0 || frames > 48000 ||
            (channels != 1 && channels != 2)) return;
        const auto now = Clock::now();
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_ || !accepting_) return;
            const size_t count = static_cast<size_t>(frames) * channels;
            if (count > maxSamples_) return;
            // Keep the newest timed audio on overload, leaving silence at the
            // missing deadlines rather than playing a burst of stale samples.
            while (!queue_.empty() && samples_ + count > maxSamples_) {
                samples_ -= queue_.front().pcm.size();
                queue_.pop_front();
            }
            queue_.push_back({std::vector<int16_t>(pcm, pcm + count), frames, channels, now + delay_});
            samples_ += count;
        }
        cv_.notify_one();
    }

    size_t queuedBytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return samples_ * sizeof(int16_t);
    }

private:
    struct Packet {
        std::vector<int16_t> pcm;
        int frames, channels;
        Clock::time_point due;
    };
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        while (!stopped_) {
            if (queue_.empty()) {
                cv_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
                continue;
            }
            const auto due = queue_.front().due;
            if (Clock::now() < due) {
                cv_.wait_until(lock, due);
                continue;
            }
            auto packet = std::move(queue_.front());
            const auto generation = generation_;
            queue_.pop_front();
            samples_ -= packet.pcm.size();
            // A suspended/overloaded client must not replay seconds of sound.
            if (Clock::now() - due > std::chrono::milliseconds(100)) continue;
            lock.unlock();
            {
                std::lock_guard<std::mutex> outputLock(outputMutex_);
                bool deliver = false;
                {
                    std::lock_guard<std::mutex> queueLock(mutex_);
                    deliver = !stopped_ && accepting_ && generation == generation_;
                }
                if (deliver && handler_) handler_(packet.pcm.data(), packet.frames, packet.channels, 48000);
            }
            lock.lock();
        }
    }
    mutable std::mutex mutex_;
    std::mutex outputMutex_;
    std::condition_variable cv_;
    std::thread thread_;
    std::deque<Packet> queue_;
    Handler handler_;
    std::chrono::milliseconds delay_{0};
    size_t samples_ = 0, maxSamples_ = 0;
    uint64_t generation_ = 0;
    bool stopped_ = true, accepting_ = false;
};

} // namespace ts3stream::media
