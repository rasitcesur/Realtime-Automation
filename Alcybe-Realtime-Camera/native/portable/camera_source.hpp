#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace alcybe {
// Frames contain width * height packed ARGB pixels in native byte order.
class CameraSource {
public:
    virtual ~CameraSource() = default;
    virtual bool readFrame(uint32_t* destination, int timeoutMs, int64_t& timestampNs) = 0;
};
std::unique_ptr<CameraSource> openCameraSource(
    const std::string& deviceId, int width, int height, int targetFps, std::string& errorMessage);
}
