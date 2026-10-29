#pragma once
#include <cstdint>
#include <cstddef>

namespace alcybe_camera {
inline uint8_t clamp(int value) {
    return static_cast<uint8_t>(value < 0 ? 0 : (value > 255 ? 255 : value));
}
// V4L2 YUYV is Y0 U Y1 V. Output is 0xFFRRGGBB, not a GPU texture or zero-copy Java raster.
inline void yuyvToArgb(const uint8_t* source, size_t stride, uint32_t* target,
                       int width, int height) {
    for (int row = 0; row < height; ++row) {
        const uint8_t* p = source + static_cast<size_t>(row) * stride;
        uint32_t* output = target + static_cast<size_t>(row) * width;
        for (int x = 0; x < width; x += 2, p += 4) {
            const int c0 = (p[0] > 16) ? p[0] - 16 : 0;
            const int c1 = (p[2] > 16) ? p[2] - 16 : 0;
            const int u = static_cast<int>(p[1]) - 128;
            const int v = static_cast<int>(p[3]) - 128;
            const int r = 409 * v + 128;
            const int g = -100 * u - 208 * v + 128;
            const int b = 516 * u + 128;
            output[x] = 0xFF000000u | (uint32_t(clamp((298 * c0 + r) >> 8)) << 16)
                     | (uint32_t(clamp((298 * c0 + g) >> 8)) << 8) | clamp((298 * c0 + b) >> 8);
            output[x + 1] = 0xFF000000u | (uint32_t(clamp((298 * c1 + r) >> 8)) << 16)
                         | (uint32_t(clamp((298 * c1 + g) >> 8)) << 8) | clamp((298 * c1 + b) >> 8);
        }
    }
}
}
