#include "media/desktop_frame.h"
#include <cstdio>

using namespace ts3stream::media;

int main() {
    int failures = 0;
    auto check = [&](bool ok, const char* message) {
        if (!ok) { ++failures; std::printf("FAIL: %s\n", message); }
    };
    // 3x2 image, including row padding that must never become visible pixels.
    const uint8_t source[] = {
        1, 0, 0, 255, 2, 0, 0, 255, 3, 0, 0, 255, 99, 99, 99, 99,
        4, 0, 0, 255, 5, 0, 0, 255, 6, 0, 0, 255, 99, 99, 99, 99
    };
    const DXGI_MODE_ROTATION rotations[] = {DXGI_MODE_ROTATION_IDENTITY,
        DXGI_MODE_ROTATION_ROTATE90, DXGI_MODE_ROTATION_ROTATE180, DXGI_MODE_ROTATION_ROTATE270};
    const uint8_t expected[][6] = {{1, 2, 3, 4, 5, 6}, {4, 1, 5, 2, 6, 3},
                                  {6, 5, 4, 3, 2, 1}, {3, 6, 2, 5, 1, 4}};
    for (int r = 0; r < 4; ++r) {
        auto frame = detail::copyDesktopFrame(source, 3, 2, 16, rotations[r]);
        check(frame.width == (r % 2 ? 2 : 3) && frame.height == (r % 2 ? 3 : 2), "rotation dimensions");
        check(frame.stride == frame.width * 4 && frame.bgra.size() == 24, "packed output stride");
        for (int i = 0; i < 6; ++i) {
            check(frame.bgra[i * 4] == expected[r][i] && frame.bgra[i * 4 + 3] == 255,
                  "rotation pixels and alpha");
        }
    }
    auto frame = detail::copyDesktopFrame(source, 3, 2, 16, DXGI_MODE_ROTATION_UNSPECIFIED);
    check(frame.bgra[12] == 4, "unspecified rotation is identity");
    DXGI_OUTDUPL_POINTER_SHAPE_INFO shape{};
    shape.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    shape.Width = 2; shape.Height = 1; shape.Pitch = 8;
    shape.HotSpot = {1, 0}; // Must not be subtracted from the supplied top-left.
    detail::drawDesktopPointer(frame, {1, 1}, shape, {200, 100, 50, 255, 100, 200, 0, 128});
    check(frame.bgra[16] == 200 && frame.bgra[17] == 100 && frame.bgra[18] == 50,
          "opaque color pointer at top-left, hotspot ignored");
    check(frame.bgra[20] == 53 && frame.bgra[21] == 100 && frame.bgra[23] == 255,
          "alpha-blended pointer");
    frame = detail::copyDesktopFrame(source, 3, 2, 16, DXGI_MODE_ROTATION_IDENTITY);
    detail::drawDesktopPointer(frame, {-1, 0}, shape, {200, 100, 50, 255, 100, 200, 0, 255});
    check(frame.bgra[0] == 100 && frame.bgra[4] == 2, "pointer clips negative coordinates");
    const auto unchanged = frame.bgra;
    detail::drawDesktopPointer(frame, {100, 100}, shape, {200, 100, 50, 255, 100, 200, 0, 255});
    detail::drawDesktopPointer(frame, {0, 0}, shape, {1});
    check(frame.bgra == unchanged, "offscreen and incomplete pointers are ignored");
    shape.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
    detail::drawDesktopPointer(frame, {0, 1}, shape, {10, 20, 30, 0, 255, 0, 255, 255});
    check(frame.bgra[12] == 10 && frame.bgra[13] == 20 && frame.bgra[14] == 30,
          "masked color replace");
    check(frame.bgra[16] == (5 ^ 255) && frame.bgra[18] == 255, "masked color XOR");
    // Four monochrome pixels exercise all AND/XOR combinations.
    frame.width = 4; frame.height = 1; frame.stride = 16; frame.bgra.assign(16, 60);
    shape.Type = DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    shape.Width = 4; shape.Height = 2; shape.Pitch = 1;
    detail::drawDesktopPointer(frame, {0, 0}, shape, {0x30, 0x50});
    check(frame.bgra[0] == 0 && frame.bgra[4] == 255 && frame.bgra[8] == 60 && frame.bgra[12] == 195,
          "monochrome black/white/transparent/inverted pointer");
    std::printf("desktop rotation and pointer composition: failures=%d\n", failures);
    return failures ? 1 : 0;
}
