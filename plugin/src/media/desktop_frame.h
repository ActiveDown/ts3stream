#pragma once

#include "wgc_capture.h"
#include <dxgi1_2.h>
#include <algorithm>
#include <cstring>

namespace ts3stream::media::detail {

// DXGI supplies an unrotated BGRA surface, even for portrait monitors.
inline CapturedVideoFrame copyDesktopFrame(const uint8_t* pixels, int width, int height,
                                           int pitch, DXGI_MODE_ROTATION rotation) {
    const bool portrait = rotation == DXGI_MODE_ROTATION_ROTATE90 ||
                          rotation == DXGI_MODE_ROTATION_ROTATE270;
    CapturedVideoFrame frame;
    frame.width = portrait ? height : width;
    frame.height = portrait ? width : height;
    frame.stride = frame.width * 4;
    frame.bgra.resize(static_cast<size_t>(frame.stride) * frame.height);
    for (int y = 0; y < height; ++y) {
        const auto* row = pixels + static_cast<size_t>(y) * pitch;
        if (rotation == DXGI_MODE_ROTATION_IDENTITY || rotation == DXGI_MODE_ROTATION_UNSPECIFIED) {
            std::memcpy(frame.bgra.data() + static_cast<size_t>(y) * frame.stride, row, frame.stride);
            continue;
        }
        for (int x = 0; x < width; ++x) {
            int dx = x, dy = y;
            if (rotation == DXGI_MODE_ROTATION_ROTATE90) { dx = height - 1 - y; dy = x; }
            if (rotation == DXGI_MODE_ROTATION_ROTATE180) { dx = width - 1 - x; dy = height - 1 - y; }
            if (rotation == DXGI_MODE_ROTATION_ROTATE270) { dx = y; dy = width - 1 - x; }
            std::memcpy(frame.bgra.data() + static_cast<size_t>(dy) * frame.stride + dx * 4,
                        row + x * 4, 4);
        }
    }
    return frame;
}

// Position is already the pointer's top-left in monitor coordinates (not its
// hotspot). Composite only when DXGI says the pointer is a separate overlay.
inline void drawDesktopPointer(CapturedVideoFrame& frame, POINT position,
                               const DXGI_OUTDUPL_POINTER_SHAPE_INFO& shape,
                               const std::vector<uint8_t>& pixels) {
    const bool mono = shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME;
    const bool color = shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR;
    const bool masked = shape.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR;
    if ((!mono && !color && !masked) || shape.Pitch == 0 ||
        pixels.size() < static_cast<size_t>(shape.Pitch) * shape.Height) return;
    const int width = static_cast<int>(shape.Width);
    const int height = static_cast<int>(mono ? shape.Height / 2 : shape.Height);
    if (shape.Pitch < (mono ? (shape.Width + 7) / 8 : shape.Width * 4)) return;
    for (int y = std::max(0, -static_cast<int>(position.y));
         y < height && position.y + y < frame.height; ++y) {
        for (int x = std::max(0, -static_cast<int>(position.x));
             x < width && position.x + x < frame.width; ++x) {
            auto* dest = frame.bgra.data() + static_cast<size_t>(position.y + y) * frame.stride +
                         static_cast<size_t>(position.x + x) * 4;
            if (mono) {
                const uint8_t bit = static_cast<uint8_t>(0x80 >> (x % 8));
                const bool andBit = (pixels[static_cast<size_t>(y) * shape.Pitch + x / 8] & bit) != 0;
                const bool xorBit = (pixels[static_cast<size_t>(y + height) * shape.Pitch + x / 8] & bit) != 0;
                for (int c = 0; c < 3; ++c) dest[c] = (andBit ? dest[c] : 0) ^ (xorBit ? 255 : 0);
            } else {
                const auto* src = pixels.data() + static_cast<size_t>(y) * shape.Pitch + x * 4;
                for (int c = 0; c < 3; ++c) {
                    dest[c] = masked ? (src[3] ? dest[c] ^ src[c] : src[c]) :
                        static_cast<uint8_t>((src[c] * src[3] + dest[c] * (255 - src[3]) + 127) / 255);
                }
            }
            dest[3] = 255;
        }
    }
}

} // namespace ts3stream::media::detail
