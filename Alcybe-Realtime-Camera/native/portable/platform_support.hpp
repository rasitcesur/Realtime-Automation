#pragma once
#include <cstddef>
#include <cstdint>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>
#include <thread>
#ifdef _WIN32
#include <windows.h>
#include <malloc.h>
#include <avrt.h>
#elif defined(__APPLE__)
#include <pthread.h>
#include <pthread/qos.h>
#else
#include <pthread.h>
#endif

namespace alcybe {
inline int64_t nowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline void* allocateAligned(size_t length) {
#ifdef _WIN32
    return _aligned_malloc(length, 64);
#else
    void* pointer = nullptr;
    return posix_memalign(&pointer, 64, length) == 0 ? pointer : nullptr;
#endif
}
inline void* allocateOnNuma(size_t length, int numaNode) {
#ifdef _WIN32
    return VirtualAllocExNuma(GetCurrentProcess(), nullptr, length,
        MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE, static_cast<DWORD>(numaNode));
#else
    (void)length;
    (void)numaNode;
    return nullptr;
#endif
}
inline void releaseOnNuma(void* pointer) {
#ifdef _WIN32
    VirtualFree(pointer, 0, MEM_RELEASE);
#else
    (void)pointer;
#endif
}
inline bool numaNodeAvailable(int numaNode) {
#ifdef _WIN32
    ULONG maximumNode = 0;
    return numaNode >= 0 && GetNumaHighestNodeNumber(&maximumNode)
        && static_cast<ULONG>(numaNode) <= maximumNode;
#else
    (void)numaNode;
    return false;
#endif
}
inline void releaseAligned(void* pointer) {
#ifdef _WIN32
    _aligned_free(pointer);
#else
    free(pointer);
#endif
}
inline void configureThread(int priority, int cpuAffinity) {
#ifdef _WIN32
    if (cpuAffinity >= 0 && cpuAffinity < static_cast<int>(sizeof(DWORD_PTR) * 8)) {
        if (!SetThreadAffinityMask(GetCurrentThread(), DWORD_PTR(1) << cpuAffinity))
            std::fprintf(stderr, "[alcybe] Could not set CPU affinity.\n");
    }
    if (priority > 0) {
        // MMCSS is a priority hint, not a scheduling deadline.
        DWORD taskIndex = 0;
        HANDLE taskHandle = AvSetMmThreadCharacteristicsW(L"Capture", &taskIndex);
        if (taskHandle) {
            AvSetMmThreadPriority(taskHandle, AVRT_PRIORITY_HIGH);
            // Keep the MMCSS registration until the worker exits.
            // A thread-local holder releases it at thread exit.
            struct TaskRegistration {
                HANDLE handle = nullptr;
                ~TaskRegistration() { if (handle) AvRevertMmThreadCharacteristics(handle); }
            };
            thread_local TaskRegistration registration;
            registration.handle = taskHandle;
        } else {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            std::fprintf(stderr, "[alcybe] MMCSS unavailable. Using thread priority.\n");
        }
    }
#elif defined(__APPLE__)
    if (cpuAffinity >= 0)
        std::fprintf(stderr, "[alcybe] CPU pinning is not available on macOS.\n");
    if (priority > 0) {
        const int status = pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        if (status != 0) std::fprintf(stderr, "[alcybe] Could not set QoS: %d\n", status);
    }
#else
    (void)priority;
    (void)cpuAffinity;
#endif
}
}
