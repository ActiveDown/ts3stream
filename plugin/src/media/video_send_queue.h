#pragma once

#include "wgc_capture.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <utility>

namespace ts3stream::media {

// One pending frame, never a FIFO of stale images. The encoder consumes at a
// bounded cadence even if capture/conversion delivers a burst after a stall.
class VideoSendQueue {
public:
    void push(std::shared_ptr<CapturedVideoFrame> frame) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_) return;
            pending_ = std::move(frame);
        }
        changed_.notify_one();
    }

    std::shared_ptr<CapturedVideoFrame> take() {
        std::unique_lock<std::mutex> lock(mutex_);
        // Resend an idle source at least every 250 ms for late subscribers.
        changed_.wait_for(lock, std::chrono::milliseconds(250), [this] { return stopped_ || pending_; });
        if (stopped_) return {};
        changed_.wait_until(lock, nextSendAt_, [this] { return stopped_; });
        if (stopped_) return {};
        return std::exchange(pending_, {});
    }

    void markSent(int fps) {
        std::lock_guard<std::mutex> lock(mutex_);
        nextSendAt_ = std::chrono::steady_clock::now() +
                     std::chrono::microseconds(1000000 / std::max(1, fps));
    }

    void stop() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            stopped_ = true;
            pending_.reset();
        }
        changed_.notify_all();
    }

    bool stopped() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return stopped_;
    }

    // Only call after joining the old consumer thread.
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        pending_.reset();
        stopped_ = false;
        nextSendAt_ = {};
    }

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::shared_ptr<CapturedVideoFrame> pending_;
    std::chrono::steady_clock::time_point nextSendAt_{};
    bool stopped_ = false;
};

} // namespace ts3stream::media
