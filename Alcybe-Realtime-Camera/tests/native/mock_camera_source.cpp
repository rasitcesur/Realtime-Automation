#include "../../native/portable/camera_source.hpp"
#include "../../native/portable/platform_support.hpp"
#include <chrono>
#include <memory>
#include <string>
#include <thread>

namespace alcybe {
class MockCameraSource final : public CameraSource {
public:
    MockCameraSource(int imageWidth, int imageHeight) : width(imageWidth), height(imageHeight) {}
    bool readFrame(uint32_t* destination, int timeoutMs, int64_t& timestampNs) override {
        if (timeoutMs <= 0) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x)
                destination[y * width + x] = 0xff000000u | (uint32_t(x & 255) << 16)
                    | (uint32_t(y & 255) << 8) | uint32_t((x + y) & 255);
        }
        timestampNs = nowNs();
        return true;
    }
private:
    int width;
    int height;
};
std::unique_ptr<CameraSource> openCameraSource(
    const std::string& deviceId, int width, int height, int targetFps, std::string& errorMessage) {
    (void)targetFps;
    if (deviceId != "mock") {
        errorMessage = "Unknown mock camera device.";
        return nullptr;
    }
    return std::make_unique<MockCameraSource>(width, height);
}
}
