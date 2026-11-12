#pragma once

#include <algorithm>
#include <charconv>
#include <cstring>

namespace ts3stream::media {

constexpr int kMinVideoBitrateKbps = 1000;
constexpr int kMaxVideoBitrateKbps = 10000;
constexpr int kDefaultVideoBitrateKbps = 4000;

// An explicit picker selection wins over the legacy diagnostic environment.
// Zero is reserved for test/tool callers that use the environment override.
inline int videoBitrateBps(int selectedKbps, const char* diagnosticOverride = nullptr) {
    if (selectedKbps > 0) {
        return std::clamp(selectedKbps, kMinVideoBitrateKbps, kMaxVideoBitrateKbps) * 1000;
    }
    if (diagnosticOverride && *diagnosticOverride) {
        int parsed = 0;
        const char* end = diagnosticOverride + std::strlen(diagnosticOverride);
        const auto result = std::from_chars(diagnosticOverride, end, parsed);
        if (result.ec == std::errc{} && result.ptr == end && parsed >= 1'000'000 && parsed <= 50'000'000) {
            return parsed;
        }
    }
    return kDefaultVideoBitrateKbps * 1000;
}

} // namespace ts3stream::media
