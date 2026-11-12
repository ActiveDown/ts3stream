#include "media/video_send_queue.h"
#include <cstdio>
#include <future>
#include <thread>

using namespace std::chrono;
using ts3stream::media::CapturedVideoFrame;
using ts3stream::media::VideoSendQueue;

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    auto frame = [](int id) {
        auto result = std::make_shared<CapturedVideoFrame>();
        result->timestampUs = id;
        return result;
    };
    VideoSendQueue queue;
    std::weak_ptr<CapturedVideoFrame> stale;
    {
        auto first = frame(1);
        stale = first;
        queue.push(std::move(first));
    }
    for (int id = 2; id <= 1000; ++id) queue.push(frame(id));
    check(stale.expired(), "burst does not retain old frame allocations");
    check(queue.take()->timestampUs == 1000, "burst delivers newest frame, not stale FIFO");

    // Emulate a slow conversion, then start pacing only at the actual send.
    queue.markSent(10);
    const auto sentAt = steady_clock::now();
    queue.push(frame(1001));
    auto taking = std::async(std::launch::async, [&] { return queue.take(); });
    check(taking.wait_for(milliseconds(20)) == std::future_status::timeout,
          "next frame cannot immediately burst into encoder");
    for (int id = 1002; id <= 2000; ++id) queue.push(frame(id));
    const auto latest = taking.get();
    check(latest && latest->timestampUs == 2000, "new frames replace pending during pacing wait");
    check(steady_clock::now() - sentAt >= milliseconds(90), "encoder input observes selected cadence");

    queue.markSent(1); // A long pacing deadline must not delay stream stop.
    queue.push(frame(2001));
    auto stopping = std::async(std::launch::async, [&] { return queue.take(); });
    queue.stop();
    check(stopping.wait_for(milliseconds(200)) == std::future_status::ready, "stop interrupts pacing wait");
    check(!stopping.get() && queue.stopped(), "stop discards pending frame");
    queue.push(frame(2002));
    check(!queue.take(), "stopped queue rejects incoming frames");
    queue.reset();
    queue.push(frame(3000));
    check(queue.take()->timestampUs == 3000 && !queue.stopped(), "restart has no stale frame/deadline");

    const auto idleAt = steady_clock::now();
    check(!queue.take(), "idle timeout requests reuse of last encoded frame");
    check(steady_clock::now() - idleAt >= milliseconds(240), "idle keepalive is not a busy loop");
    auto idleStop = std::async(std::launch::async, [&] { return queue.take(); });
    queue.stop();
    check(idleStop.wait_for(milliseconds(200)) == std::future_status::ready && !idleStop.get(),
          "stop also interrupts idle wait");
    std::printf("video sender burst pacing and bounded queue: failures=%d\n", failures);
    return failures ? 1 : 0;
}
