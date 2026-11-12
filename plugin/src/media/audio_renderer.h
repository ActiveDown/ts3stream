#pragma once

#include <cstdint>
#include <cstddef>
#include <memory>

namespace ts3stream::media {

// Minimal waveOut-based renderer for the received LiveKit audio track.
// Accepts 48 kHz mono or stereo int16 and always plays 48 kHz stereo.
class AudioRenderer {
public:
    AudioRenderer();
    ~AudioRenderer();

    AudioRenderer(const AudioRenderer&) = delete;
    AudioRenderer& operator=(const AudioRenderer&) = delete;

    bool start(int sampleRate, int channels);
    void stop();
    void setVolume(int percent);

    void push(const int16_t* samples, int frames, int channels);

    struct Stats {
        size_t queuedFrames;
        uint64_t droppedFrames;
        uint64_t submittedFrames;
        uint64_t outputErrors;
    };
    Stats stats() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace ts3stream::media
