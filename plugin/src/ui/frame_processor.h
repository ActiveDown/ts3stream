#pragma once

#include "media/wgc_capture.h"

#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>

namespace ts3stream::ui {

// Serializes expensive per-frame work (QImage conversion, scaling, encoding
// hand-off) away from the WGC callback thread. Only the newest frame is kept.
class FrameProcessor {
public:
    using Handler = std::function<void(const media::CapturedVideoFrame&)>;

    explicit FrameProcessor(Handler handler);
    ~FrameProcessor();

    FrameProcessor(const FrameProcessor&) = delete;
    FrameProcessor& operator=(const FrameProcessor&) = delete;

    void start();
    void stop();
    void push(media::CapturedVideoFrame frame);

private:
    void run();

    Handler handler_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::shared_ptr<media::CapturedVideoFrame> pending_;
    bool stop_ = false;
    bool running_ = false;
};

} // namespace ts3stream::ui
