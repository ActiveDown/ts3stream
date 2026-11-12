#include "media/audio_playout_queue.h"
#include <array>
#include <cstdio>

int main() {
    using namespace ts3stream::media;
    using namespace std::chrono;
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    AudioPlayoutQueue queue;
    const AudioPlayoutQueue::Clock::time_point start{};
    const int16_t stereo[]{1, -1, 2, -2, 3, -3};
    const int16_t mono[]{4, 5};
    std::array<int16_t, 12> out{};
    queue.push(stereo, 3, 2, start);
    queue.push(mono, 2, 1, start);
    check(queue.read(out.data(), 2, start) == 2 && out[0] == 1 && out[3] == -2, "partial stereo read");
    check(queue.read(out.data(), 6, start) == 3 && out[0] == 3 && out[1] == -3 &&
          out[2] == 4 && out[3] == 4 && out[4] == 5 && out[5] == 5 && out[6] == 0 && out[11] == 0,
          "channel alignment, mono conversion, FIFO and underrun silence");
    queue.push(stereo, 3, 2, start);
    check(queue.read(out.data(), 6, start + seconds(1)) == 0 && out[0] == 0 && queue.droppedFrames() == 3,
          "discard stale audio after suspend even with no further arrivals");
    queue.clear();
    std::vector<int16_t> burst(48000 * 2);
    for (size_t i = 0; i < burst.size() / 2; ++i) {
        burst[2 * i] = static_cast<int16_t>(i / 10);
        burst[2 * i + 1] = -burst[2 * i];
    }
    queue.push(burst.data(), 48000, 2, start);
    check(queue.queuedFrames() == AudioPlayoutQueue::kMaxFrames, "oversized burst bounded before allocation");
    queue.read(out.data(), 1, start);
    check(out[0] == (48000 - AudioPlayoutQueue::kMaxFrames) / 10 && out[1] == -out[0], "keep newest aligned tail");

    // Five simulated minutes: capture clock 10 ms, device consumes 20 ms every
    // 21 ms, with periodic scheduler stalls. The old FIFO reaches 2 seconds.
    queue.clear();
    std::vector<int16_t> packet(480 * 2), rendered(960 * 2);
    size_t legacyFrames = 0, legacyMax = 0, maxQueued = 0;
    int maxAge = 0;
    for (int ms = 0; ms < 300000; ++ms) {
        const auto now = start + milliseconds(ms);
        if (ms % 10 == 0) {
            std::fill(packet.begin(), packet.end(), static_cast<int16_t>(ms / 10));
            queue.push(packet.data(), 480, 2, now);
            legacyFrames = std::min(size_t{96000}, legacyFrames + 480);
        }
        maxQueued = std::max(maxQueued, queue.queuedFrames());
        legacyMax = std::max(legacyMax, legacyFrames);
        if (ms % 21 == 0 && ms % 5000 >= 250) {
            const auto frames = queue.read(rendered.data(), 960, now);
            legacyFrames -= std::min(legacyFrames, size_t{960});
            for (size_t i = 0; i < frames; ++i) maxAge = std::max(maxAge, ms - rendered[i * 2] * 10);
        }
    }
    check(legacyMax == 96000, "fixture reproduces legacy two-second buildup");
    check(maxQueued <= AudioPlayoutQueue::kMaxFrames && maxAge <= 100 && queue.droppedFrames() > 0,
          "long-running drift/stalls cannot accumulate stale PCM");
    std::printf("simulated_seconds=300 legacy_max_ms=%.1f bounded_max_ms=%.1f max_played_age_ms=%d\n",
        legacyMax / 48.0, maxQueued / 48.0, maxAge);
    queue.clear();
    check(queue.queuedFrames() == 0 && queue.droppedFrames() == 0, "restart clears audio and counters");
    AudioBufferOrder order;
    std::array<bool, 4> done{true, true, true, false};
    std::vector<int> submitted;
    auto refill = [&] { order.refill(4, [&](int i) { return done[i]; }, [&](int i) {
        submitted.push_back(i); done[i] = false; return true;
    }); };
    refill();
    done = {true, false, false, true};
    refill();
    check(submitted == std::vector<int>({0, 1, 2, 3, 0}), "coalesced wraparound completions preserve playback order");
    done[1] = true;
    order.refill(4, [&](int i) { return done[i]; }, [](int) { return false; });
    refill();
    check(submitted.back() == 1, "failed write does not advance ring");
    std::printf("audio playout queue and device order: failures=%d\n", failures);
    return failures ? 1 : 0;
}
