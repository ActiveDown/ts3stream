#pragma once

#include <algorithm>
#include <cstdint>

namespace ts3stream::media {

// Software gain on received PCM only; never change the shared Windows/TS3 mixer.
inline int16_t applyPlaybackGain(int16_t sample, int percent) {
    return static_cast<int16_t>(static_cast<int32_t>(sample) * std::clamp(percent, 0, 100) / 100);
}

} // namespace ts3stream::media
