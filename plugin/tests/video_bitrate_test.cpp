#include "media/video_bitrate.h"
#include <cstdio>

int main() {
    using ts3stream::media::videoBitrateBps;
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    check(videoBitrateBps(0) == 4'000'000, "default 4 Mbps");
    for (int kbps = 1000; kbps <= 10000; kbps += 1000) {
        check(videoBitrateBps(kbps) == kbps * 1000, "all picker values converted to bits/s");
        check(videoBitrateBps(kbps, "4000000") == kbps * 1000, "explicit UI value wins over diagnostic override");
    }
    check(videoBitrateBps(500) == 1'000'000 && videoBitrateBps(20000) == 10'000'000, "explicit value bounded");
    check(videoBitrateBps(0, "4000000") == 4'000'000, "legacy diagnostic override works for tools");
    for (const char* invalid : {"", "-1000000", "999999", "50000001", "4000000oops", "9999999999999999999999"}) {
        check(videoBitrateBps(0, invalid) == 4'000'000, "invalid diagnostic setting falls back safely");
    }
    std::printf("video bitrate: failures=%d\n", failures);
    return failures ? 1 : 0;
}
