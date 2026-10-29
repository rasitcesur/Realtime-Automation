#include "camera_pixel.hpp"
#include "alcybe_realtime_RealtimeCamera.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    static_assert(offsetof(SharedCameraFrameBlock, payload) == 64, "ABI error");
    // Black, white, near-black and luma pattern (with neutral chroma).
    std::array<uint8_t, 16> yuyv{
        16, 128, 235, 128, 16, 128, 16, 128,
        235, 128, 16, 128, 100, 128, 200, 128
    };
    std::array<uint32_t, 4> rgb{};
    alcybe_camera::yuyvToArgb(yuyv.data(), 8, rgb.data(), 2, 2);
    assert(rgb[0] == 0xff000000u);
    assert(rgb[1] == 0xffffffffu);
    assert(rgb[2] == 0xffffffffu);
    assert(rgb[3] == 0xff000000u);
    // Verify row stride and that stride padding is not decoded.
    std::array<uint8_t, 12> padded{16, 128, 235, 128, 255, 255, 255, 255, 235, 128, 16, 128};
    alcybe_camera::yuyvToArgb(padded.data(), 8, rgb.data(), 2, 2);
    assert(rgb[0] == 0xff000000u && rgb[1] == 0xffffffffu);
    assert(rgb[2] == 0xffffffffu && rgb[3] == 0xff000000u);
    assert(alcybe_camera::clamp(-7) == 0 && alcybe_camera::clamp(400) == 255);
    std::cout << "PASS: camera YUYV/ARGB and stride unit tests\n";
}
