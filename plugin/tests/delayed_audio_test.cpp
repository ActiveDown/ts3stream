#include "media/delayed_audio.h"
#include <atomic>
#include <cstdio>

int main() {
    using namespace ts3stream::media;
    int failures = 0;
    auto check = [&](bool ok, const char* description) {
        if (!ok) { ++failures; std::printf("FAIL: %s\n", description); }
    };
    check(roomAudioDelayMs("ts3stream-av-delay-v1:2000") == 2000, "room contract");
    for (const auto& text : {"", "2000", "ts3stream-av-delay-v1:-1", "ts3stream-av-delay-v1:6000",
                            "ts3stream-av-delay-v1:2001", "ts3stream-av-delay-v1:2000extra",
                            "ts3stream-av-delay-v1:99999999999999999"}) {
        check(roomAudioDelayMs(text) == 0, "unknown/invalid metadata stays unbuffered");
    }
    std::mutex mutex;
    std::condition_variable cv;
    std::vector<int> received;
    std::vector<double> delays;
    auto start = DelayedAudio::Clock::now();
    DelayedAudio delayed;
    delayed.start(200, [&](const int16_t* samples, int frames, int channels, int rate) {
        std::lock_guard<std::mutex> lock(mutex);
        received.push_back(samples[0]);
        delays.push_back(std::chrono::duration<double, std::milli>(DelayedAudio::Clock::now() - start).count());
        if (frames != 480 || channels != 2 || rate != 48000) ++failures;
        cv.notify_all();
    });
    std::vector<int16_t> pcm(960, 123);
    delayed.push(pcm.data(), 480, 2, 48000);
    pcm.assign(960, -456);
    delayed.push(pcm.data(), 480, 2, 48000);
    pcm.assign(960, 0);
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(!cv.wait_for(lock, std::chrono::milliseconds(100), [&] { return !received.empty(); }), "no early sound");
        check(cv.wait_for(lock, std::chrono::seconds(2), [&] { return received.size() == 2; }), "delayed sound arrives");
        check(received == std::vector<int>({123, -456}), "PCM copied and ordered without gain/clipping");
        check(!delays.empty() && delays.front() >= 190 && delays.front() < 500, "monotonic deadline");
    }
    delayed.stop();
    std::atomic<int> calls{0};
    delayed.start(5000, [&](const int16_t*, int, int, int) { ++calls; });
    for (int i = 0; i < 2000; ++i) delayed.push(pcm.data(), 480, 2, 48000);
    check(delayed.queuedBytes() > 0 && delayed.queuedBytes() <= 48000u * 2 * 2 * 5500 / 1000,
          "bounded PCM allocation under overload");
    delayed.setAccepting(false);
    check(delayed.queuedBytes() == 0, "reconnect clears old audio");
    delayed.push(pcm.data(), 480, 2, 48000);
    check(delayed.queuedBytes() == 0, "suspended stream rejects audio");
    delayed.setAccepting(true);
    delayed.push(pcm.data(), 480, 2, 48000);
    check(delayed.queuedBytes() > 0, "resubscription resumes accepting");
    start = DelayedAudio::Clock::now();
    delayed.stop();
    check(DelayedAudio::Clock::now() - start < std::chrono::milliseconds(200), "stop interrupts long deadline");
    check(delayed.queuedBytes() == 0 && calls.load() == 0, "stop discards tail and callbacks");
    delayed.start(0, [&](const int16_t*, int, int, int) { ++calls; cv.notify_all(); });
    delayed.push(pcm.data(), 480, 2, 48000);
    {
        std::unique_lock<std::mutex> lock(mutex);
        check(cv.wait_for(lock, std::chrono::seconds(1), [&] { return calls.load() == 1; }), "fresh restart without stale tail");
    }
    delayed.stop();
    std::printf("delayed audio deadlines, bounds and lifecycle: failures=%d\n", failures);
    return failures ? 1 : 0;
}
