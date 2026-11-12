#include "media/audio_renderer.h"
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

// Silent native-device smoke test: no capture and no audible test tone.
int main() {
    using namespace std::chrono;
    ts3stream::media::AudioRenderer renderer;
    renderer.setVolume(0);
    if (!renderer.start(48000, 2)) {
        std::fprintf(stderr, "No waveOut output device available\n"); return 77;
    }
    std::atomic<bool> producing{true};
    std::thread producer([&] {
        std::array<int16_t, 960> silence{};
        auto due = steady_clock::now();
        while (producing.load()) {
            renderer.push(silence.data(), 480, 2);
            due += milliseconds(10);
            std::this_thread::sleep_until(due);
        }
    });
    std::this_thread::sleep_for(seconds(2));
    std::vector<int16_t> burst(48000 * 2);
    renderer.push(burst.data(), 48000, 2);
    std::this_thread::sleep_for(seconds(3));
    const auto stats = renderer.stats();
    const auto before = steady_clock::now();
    renderer.stop(); // exercise concurrent callback/stop without refilling after reset
    const auto stopTime = steady_clock::now() - before;
    producing.store(false);
    producer.join();
    renderer.push(burst.data(), 48000, 2);
    bool ok = stats.outputErrors == 0 && stats.submittedFrames > 48000 * 4 &&
        stats.queuedFrames <= 3840 && stats.droppedFrames > 0 &&
        stopTime < milliseconds(500) && renderer.stats().queuedFrames == 0;
    ok = renderer.start(48000, 1) && ok;
    std::this_thread::sleep_for(milliseconds(150));
    ok = renderer.stats().outputErrors == 0 && renderer.stats().submittedFrames > 0 && ok;
    renderer.stop();
    std::printf("silent native waveOut submitted_frames=%llu dropped_frames=%llu stop_ms=%.1f %s\n",
        static_cast<unsigned long long>(stats.submittedFrames), static_cast<unsigned long long>(stats.droppedFrames),
        duration<double, std::milli>(stopTime).count(), ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
