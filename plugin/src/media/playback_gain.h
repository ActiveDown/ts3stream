#pragma once

#include <algorithm>
#include <cstdint>
#include <limits>

namespace ts3stream::media {

inline constexpr int kMaxPlaybackVolumePercent = 125;

// Software gain on received PCM only; never change the shared Windows/TS3 mixer.
inline int16_t applyPlaybackGain(int16_t sample, int percent) {
    const int32_t amplified = static_cast<int32_t>(sample) *
                             std::clamp(percent, 0, kMaxPlaybackVolumePercent) / 100;
    // Saturate amplified peaks instead of wrapping to the opposite sign.
    return static_cast<int16_t>(std::clamp(amplified,
        static_cast<int32_t>(std::numeric_limits<int16_t>::min()),
        static_cast<int32_t>(std::numeric_limits<int16_t>::max())));
}

} // namespace ts3stream::media
