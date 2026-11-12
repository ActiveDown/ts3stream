#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <deque>
#include <vector>

namespace ts3stream::media {

// Device staging only, AFTER the negotiated A/V delay. Never use this queue
// as another jitter buffer. Caller serializes push/read/clear with its mutex.
class AudioPlayoutQueue {
public:
    using Clock = std::chrono::steady_clock;
    static constexpr int kRate = 48000;
    static constexpr size_t kMaxFrames = kRate * 80 / 1000;
    static constexpr size_t kRecoveryFrames = kRate * 40 / 1000;
    static constexpr auto kMaxAge = std::chrono::milliseconds(100);

    void push(const int16_t* pcm, int frames, int channels, Clock::time_point now) {
        if (!pcm || frames <= 0 || (channels != 1 && channels != 2)) return;
        expire(now);
        size_t count = static_cast<size_t>(frames);
        if (count > kMaxFrames) {
            const size_t skip = count - kMaxFrames;
            dropped_ += skip;
            pcm += skip * channels;
            count = kMaxFrames;
        }
        if (frames_ + count > kMaxFrames) {
            const size_t keep = count < kRecoveryFrames ? kRecoveryFrames - count : 0;
            discard(frames_ - std::min(frames_, keep));
        }
        Packet packet;
        packet.arrived = now;
        packet.pcm.resize(count * 2);
        for (size_t i = 0; i < count; ++i) {
            packet.pcm[i * 2] = pcm[i * channels];
            packet.pcm[i * 2 + 1] = pcm[i * channels + channels - 1];
        }
        frames_ += count;
        packets_.push_back(std::move(packet));
    }

    // Always fill the requested stereo buffer, padding underruns with silence.
    size_t read(int16_t* out, size_t frames, Clock::time_point now) {
        expire(now);
        std::fill_n(out, frames * 2, int16_t{0});
        size_t copied = 0;
        while (copied < frames && !packets_.empty()) {
            auto& packet = packets_.front();
            const size_t take = std::min(frames - copied, packet.pcm.size() / 2 - packet.offset);
            std::copy_n(packet.pcm.data() + packet.offset * 2, take * 2, out + copied * 2);
            packet.offset += take;
            copied += take;
            frames_ -= take;
            if (packet.offset == packet.pcm.size() / 2) packets_.pop_front();
        }
        return copied;
    }

    size_t queuedFrames() const { return frames_; }
    uint64_t droppedFrames() const { return dropped_; }
    double oldestMs(Clock::time_point now) const {
        return packets_.empty() ? 0 : std::chrono::duration<double, std::milli>(now - packets_.front().arrived).count();
    }
    void clear() { packets_.clear(); frames_ = 0; dropped_ = 0; }

private:
    struct Packet {
        std::vector<int16_t> pcm;
        size_t offset = 0;
        Clock::time_point arrived;
    };
    void discard(size_t frames) {
        while (frames && !packets_.empty()) {
            auto& packet = packets_.front();
            const size_t take = std::min(frames, packet.pcm.size() / 2 - packet.offset);
            packet.offset += take;
            frames -= take;
            frames_ -= take;
            dropped_ += take;
            if (packet.offset == packet.pcm.size() / 2) packets_.pop_front();
        }
    }
    void expire(Clock::time_point now) {
        while (!packets_.empty() && now - packets_.front().arrived > kMaxAge) {
            discard(packets_.front().pcm.size() / 2 - packets_.front().offset);
        }
    }
    std::deque<Packet> packets_;
    size_t frames_ = 0;
    uint64_t dropped_ = 0;
};

// waveOut completion events can coalesce across the end of the buffer ring.
// Requeue in playback order, not ascending header-index order (0,3 vs 3,0).
class AudioBufferOrder {
public:
    template<class Done, class Submit>
    void refill(int count, Done done, Submit submit) {
        for (int n = 0; n < count && done(next_); ++n) {
            if (!submit(next_)) return;
            next_ = (next_ + 1) % count;
        }
    }
private:
    int next_ = 0;
};

} // namespace ts3stream::media
