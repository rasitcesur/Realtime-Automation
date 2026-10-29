#pragma once
#include <jni.h>
#include <pthread.h>
#include <cstdint>
#include <cstddef>
#include <linux/videodev2.h>

constexpr int CAM_STATE_STOPPED = 0;
constexpr int CAM_STATE_RUNNING = 1;
constexpr int CAM_STATE_PAUSED = 2;
constexpr int PIXEL_MODE_YUYV_TO_INT_RGB = 0;
constexpr int PIXEL_MODE_RAW_MJPEG = 1;

struct MmapBuffer { void* start; size_t length; };

struct alignas(64) SharedCameraFrameBlock {
    int64_t frameSequence;
    int64_t hwTimestampNs;
    int64_t pollTimestampNs;
    int64_t dutyCycleNs;
    int64_t idleCycleNs;
    int64_t frameIntervalNs;
    int32_t payloadBytesUsed;
    int32_t cameraState;
    int32_t droppedFrames;
    int32_t gcTriggeredCount; // accepted asynchronous GC requests, not completed GC cycles
    uint8_t payload[]; // GNU C++ flexible-array extension; offset fixed at 64 bytes
};
static_assert(offsetof(SharedCameraFrameBlock, payload) == 64, "Shared header ABI changed");

struct alignas(64) RealtimeCameraContext {
    int fd;
    int width;
    int height;
    int targetFps;
    int pixelMode;
    int numaNode;
    size_t rowStride;
    size_t sharedBlockTotalSize;
    size_t payloadCapacity;
    bool numaSharedAllocation;
    bool streamOn;
    bool schedulerStarted;
    unsigned int bufferCount;
    MmapBuffer* kernelBuffers;
    SharedCameraFrameBlock* sharedBlock;
    pthread_t schedulerThread;
    jobject javaCameraInstance;
    jmethodID onFrameMethodId;
    jmethodID onIdleGcMethodId;
    int priority;
    int cpuAffinity;
    int64_t beginTimeNs;
    int64_t offsetNs;
    int64_t cycleTimeNs;
    int64_t lastFrameHwNs;
    int32_t state;
    pthread_mutex_t stateMutex;
    pthread_cond_t stateCond;
    pthread_mutex_t captureMutex;
};
