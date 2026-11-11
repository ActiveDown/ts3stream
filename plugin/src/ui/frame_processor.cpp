#include "frame_processor.h"

namespace ts3stream::ui {

FrameProcessor::FrameProcessor(Handler handler) : handler_(std::move(handler)) {}

FrameProcessor::~FrameProcessor() {
    stop();
}

void FrameProcessor::start() {
    if (running_) {
        return;
    }
    stop_ = false;
    running_ = true;
    thread_ = std::thread([this] { run(); });
}

void FrameProcessor::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        stop_ = true;
        pending_.reset();
    }
    cv_.notify_all();
    if (thread_.joinable()) {
        thread_.join();
    }
    running_ = false;
}

void FrameProcessor::push(media::CapturedVideoFrame frame) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stop_) {
            return;
        }
        pending_ = std::make_shared<media::CapturedVideoFrame>(std::move(frame));
    }
    cv_.notify_one();
}

void FrameProcessor::run() {
    while (true) {
        std::shared_ptr<media::CapturedVideoFrame> frame;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] { return stop_ || pending_ != nullptr; });
            if (stop_) {
                return;
            }
            frame = std::move(pending_);
            pending_.reset();
        }

        if (frame && handler_) {
            handler_(*frame);
        }
    }
}

} // namespace ts3stream::ui
