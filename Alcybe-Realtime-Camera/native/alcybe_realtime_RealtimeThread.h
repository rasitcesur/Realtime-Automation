#ifndef ALCYBE_REALTIME_THREAD_H
#define ALCYBE_REALTIME_THREAD_H

#include <jni.h>
#include <pthread.h>
#include <cstdint>
#include <cstddef>

constexpr size_t CACHE_LINE_SIZE = 64;
constexpr int STATE_STOPPED = 0;
constexpr int STATE_RUNNING = 1;
constexpr int STATE_PAUSED = 2;

// Layout ABI: byte offsets must match RealtimeThread.java.
struct alignas(CACHE_LINE_SIZE) SharedMemoryBlock {
    int64_t sequenceNumber;    // byte 0
    int64_t dutyCycleNs;       // byte 8
    int64_t idleCycleNs;       // byte 16
    int64_t actualJitterNs;    // byte 24
    int32_t threadState;       // byte 32
    int32_t gcTriggeredCount;  // byte 36
    uint8_t padding[24];       // payload begins at byte 64
};
static_assert(sizeof(SharedMemoryBlock) == 64, "Shared ABI header must be 64 bytes");
static_assert(offsetof(SharedMemoryBlock, threadState) == 32, "ABI layout mismatch");

struct RealtimeContext {
    pthread_t threadId{};
    bool joinable = false;
    bool useNumaBlock = false;
    bool gcEnabled = false;
    jobject javaInstance = nullptr;
    jmethodID onCycleMethodId = nullptr;
    jmethodID requestGcMethodId = nullptr;
    SharedMemoryBlock* sharedBlock = nullptr;
    size_t sharedBlockSize = 0;
    int priority = 0;
    int cpuAffinity = -1;
    int numaNode = -1;
    int mode = 0; // 1: once, 2: periodic
    int64_t beginTimeNs = 0;
    int64_t offsetNs = 0;
    int64_t cycleTimeNs = 0;
    int32_t state = STATE_STOPPED;
    pthread_mutex_t stateMutex{};
    pthread_cond_t stateCond{};  // CLOCK_MONOTONIC
};
#endif
