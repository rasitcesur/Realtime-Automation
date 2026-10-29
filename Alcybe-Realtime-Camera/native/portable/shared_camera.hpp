#pragma once
#include <cstddef>
#include <cstdint>

namespace alcybe {
constexpr int cameraStopped = 0;
constexpr int cameraRunning = 1;
constexpr int cameraPaused = 2;
constexpr int64_t gcIdleThresholdNs = 1000000LL;
struct alignas(64) SharedCameraFrame {
    int64_t frameSequence;
    int64_t hwTimestampNs;
    int64_t pollTimestampNs;
    int64_t dutyCycleNs;
    int64_t idleCycleNs;
    int64_t frameIntervalNs;
    int32_t payloadBytesUsed;
    int32_t cameraState;
    int32_t droppedFrames;
    int32_t gcTriggeredCount;
    uint8_t payload[1];
};
static_assert(offsetof(SharedCameraFrame, payload) == 64, "Camera header ABI mismatch");
}
