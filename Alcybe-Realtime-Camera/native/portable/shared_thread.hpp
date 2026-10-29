#pragma once
#include <cstddef>
#include <cstdint>
namespace alcybe {
struct alignas(64) SharedThreadFrame {
    int64_t sequenceNumber;
    int64_t dutyCycleNs;
    int64_t idleCycleNs;
    int64_t actualJitterNs;
    int32_t threadState;
    int32_t gcTriggeredCount;
    uint8_t padding[24];
};
static_assert(sizeof(SharedThreadFrame) == 64, "Thread header ABI mismatch");
}
