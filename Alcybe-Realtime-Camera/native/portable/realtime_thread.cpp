#include "platform_support.hpp"
#include "shared_thread.hpp"
#include "shared_camera.hpp"
#include "jni_support.hpp"
#include <jni.h>
#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <limits>
#include <system_error>
#include <memory>

namespace alcybe {
JavaVM* javaVm = nullptr;
constexpr int threadStopped = 0;
constexpr int threadRunning = 1;
constexpr int threadPaused = 2;

struct RealtimeContext {
    std::mutex stateMutex;
    std::condition_variable stateChanged;
    std::thread worker;
    SharedThreadFrame* sharedBlock = nullptr;
    size_t sharedSize = 0;
    bool numaAllocation = false;
    jobject javaInstance = nullptr;
    jmethodID cycleMethod = nullptr;
    jmethodID gcMethod = nullptr;
    int priority = 0;
    int cpuAffinity = -1;
    int mode = 0;
    int64_t startNs = 0;
    int64_t offsetNs = 0;
    int64_t periodNs = 0;
    int32_t state = threadStopped;
    bool gcEnabled = false;
};

struct NativeBarrier {
    std::mutex mutex;
    std::condition_variable changed;
    int members;
    int arrived = 0;
    int generation = 0;
    explicit NativeBarrier(int count) : members(count) {}
};

void workerMain(RealtimeContext* context) {
    JNIEnv* env = nullptr;
    if (javaVm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) {
        store32(&context->state, threadStopped);
        store32(&context->sharedBlock->threadState, threadStopped);
        return;
    }
    configureThread(context->priority, context->cpuAffinity);
    auto invokeCycle = [&]() {
        env->CallVoidMethod(context->javaInstance, context->cycleMethod,
            static_cast<jlong>(reinterpret_cast<intptr_t>(context->sharedBlock)));
        if (env->ExceptionCheck()) { clearException(env); return false; }
        return true;
    };
    if (context->mode == 1) {
        if (load32(&context->state) == threadRunning) {
            const int64_t startNs = nowNs();
            invokeCycle();
            store64(&context->sharedBlock->dutyCycleNs, nowNs() - startNs);
            add64(&context->sharedBlock->sequenceNumber, 1);
        }
    } else {
        int64_t targetNs = context->startNs + context->offsetNs;
        std::unique_lock<std::mutex> lock(context->stateMutex);
        while (load32(&context->state) != threadStopped) {
            if (load32(&context->state) == threadPaused) {
                store32(&context->sharedBlock->threadState, threadPaused);
                context->stateChanged.wait(lock, [&]() { return load32(&context->state) != threadPaused; });
                targetNs = nowNs() + context->periodNs;
            }
            if (load32(&context->state) == threadStopped) break;
            if (nowNs() < targetNs) {
                context->stateChanged.wait_until(lock,
                    std::chrono::steady_clock::time_point(std::chrono::nanoseconds(targetNs)),
                    [&]() { return load32(&context->state) != threadRunning; });
                continue;
            }
            lock.unlock();
            store32(&context->sharedBlock->threadState, threadRunning);
            const int64_t startNs = nowNs();
            store64(&context->sharedBlock->actualJitterNs, std::max<int64_t>(0, startNs - targetNs));
            bool succeeded = invokeCycle();
            const int64_t endNs = nowNs();
            const int64_t nextNs = targetNs + context->periodNs;
            const int64_t idleNs = std::max<int64_t>(0, nextNs - endNs);
            store64(&context->sharedBlock->dutyCycleNs, endNs - startNs);
            store64(&context->sharedBlock->idleCycleNs, idleNs);
            add64(&context->sharedBlock->sequenceNumber, 1);
            if (succeeded && context->gcEnabled && idleNs > gcIdleThresholdNs) {
                env->CallVoidMethod(context->javaInstance, context->gcMethod, static_cast<jlong>(idleNs));
                if (env->ExceptionCheck()) { clearException(env); succeeded = false; }
            }
            targetNs = nextNs;
            if (targetNs <= endNs)
                targetNs += ((endNs - targetNs) / context->periodNs + 1) * context->periodNs;
            lock.lock();
            if (!succeeded) store32(&context->state, threadStopped);
        }
    }
    store32(&context->state, threadStopped);
    store32(&context->sharedBlock->threadState, threadStopped);
    javaVm->DetachCurrentThread();
}

void launch(JNIEnv* env, RealtimeContext* context, int mode, int64_t startNs, int64_t offsetNs, int64_t periodNs) {
    if (!context) { throwJava(env, "java/lang/IllegalStateException", "Thread is closed"); return; }
    std::lock_guard<std::mutex> lock(context->stateMutex);
    if (context->worker.joinable()) {
        throwJava(env, "java/lang/IllegalStateException", "Stop and join the previous worker first"); return;
    }
    context->mode = mode;
    context->startNs = startNs;
    context->offsetNs = offsetNs;
    context->periodNs = periodNs;
    store32(&context->state, threadRunning);
    store32(&context->sharedBlock->threadState, threadRunning);
    try { context->worker = std::thread(workerMain, context); }
    catch (const std::system_error&) {
        store32(&context->state, threadStopped);
        throwJava(env, "java/lang/IllegalStateException", "Could not start worker thread");
    }
}
}

extern "C" {
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    alcybe::javaVm = vm;
    return JNI_VERSION_1_8;
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_initSystem(JNIEnv*, jclass, jint) {}
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_createContext
(JNIEnv* env, jobject instance, jint priority, jint cpuAffinity, jint numaNode, jint sharedSize, jboolean gcEnabled) {
    using namespace alcybe;
    if (priority < 0 || priority > 99 || cpuAffinity < -1 || sharedSize < 128 || sharedSize > 1 << 28 || numaNode < -1) {
        throwJava(env, "java/lang/IllegalArgumentException", "Invalid priority, CPU, NUMA node or buffer size"); return 0;
    }
    if (numaNode >= 0 && !numaNodeAvailable(numaNode)) {
        throwJava(env, "java/lang/UnsupportedOperationException", "Requested NUMA node is unavailable"); return 0;
    }
#ifdef __APPLE__
    if (cpuAffinity >= 0) { throwJava(env, "java/lang/UnsupportedOperationException", "macOS does not support CPU pinning"); return 0; }
#endif
    if (cpuAffinity >= static_cast<int>(sizeof(uintptr_t) * 8)) {
        throwJava(env, "java/lang/IllegalArgumentException", "CPU index is outside the current processor group"); return 0;
    }
    auto context = std::make_unique<RealtimeContext>();
    context->sharedSize = (static_cast<size_t>(sharedSize) + 63u) & ~size_t(63u);
    context->numaAllocation = numaNode >= 0;
    context->sharedBlock = static_cast<SharedThreadFrame*>(context->numaAllocation
        ? allocateOnNuma(context->sharedSize, numaNode) : allocateAligned(context->sharedSize));
    if (!context->sharedBlock) { throwJava(env, "java/lang/OutOfMemoryError", "Shared memory allocation failed"); return 0; }
    std::memset(context->sharedBlock, 0, context->sharedSize);
    context->javaInstance = env->NewGlobalRef(instance);
    jclass classRef = env->GetObjectClass(instance);
    context->cycleMethod = classRef ? env->GetMethodID(classRef, "onRealtimeCycle", "(J)V") : nullptr;
    context->gcMethod = classRef && !env->ExceptionCheck() ? env->GetMethodID(classRef, "onIdleGcWindow", "(J)V") : nullptr;
    if (classRef) env->DeleteLocalRef(classRef);
    if (env->ExceptionCheck() || !context->javaInstance || !context->cycleMethod || !context->gcMethod) {
        if (context->javaInstance) env->DeleteGlobalRef(context->javaInstance);
        if (context->numaAllocation) releaseOnNuma(context->sharedBlock);
        else releaseAligned(context->sharedBlock);
        return 0;
    }
    context->priority = priority;
    context->cpuAffinity = cpuAffinity;
    context->gcEnabled = (gcEnabled == JNI_TRUE);
    return static_cast<jlong>(reinterpret_cast<intptr_t>(context.release()));
}
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_getSharedMemoryAddress(JNIEnv*, jobject, jlong handle) {
    auto* context = alcybe::fromHandle<alcybe::RealtimeContext>(handle);
    return context ? static_cast<jlong>(reinterpret_cast<intptr_t>(context->sharedBlock)) : 0;
}
JNIEXPORT jobject JNICALL Java_alcybe_realtime_RealtimeThread_getSharedDirectBuffer(JNIEnv* env, jobject, jlong handle) {
    auto* context = alcybe::fromHandle<alcybe::RealtimeContext>(handle);
    return context ? env->NewDirectByteBuffer(context->sharedBlock, static_cast<jlong>(context->sharedSize)) : nullptr;
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_startRealtime(JNIEnv* env, jobject, jlong handle) {
    alcybe::launch(env, alcybe::fromHandle<alcybe::RealtimeContext>(handle), 1, 0, 0, 0);
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_scheduleRealtime
(JNIEnv* env, jobject, jlong handle, jlong beginNs, jlong offsetNs, jlong periodNs) {
    if (periodNs <= 0 || offsetNs < 0 || beginNs < 0 || beginNs > INT64_MAX - offsetNs) {
        alcybe::throwJava(env, "java/lang/IllegalArgumentException", "Invalid period or deadline"); return;
    }
    alcybe::launch(env, alcybe::fromHandle<alcybe::RealtimeContext>(handle), 2,
        beginNs ? beginNs : alcybe::nowNs(), offsetNs, periodNs);
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_pauseThread(JNIEnv*, jobject, jlong handle) {
    auto* context = alcybe::fromHandle<alcybe::RealtimeContext>(handle);
    if (!context) return;
    std::lock_guard<std::mutex> lock(context->stateMutex);
    if (context->mode == 2 && alcybe::load32(&context->state) == alcybe::threadRunning)
        alcybe::store32(&context->state, alcybe::threadPaused);
    context->stateChanged.notify_all();
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_resumeThread(JNIEnv*, jobject, jlong handle) {
    auto* context = alcybe::fromHandle<alcybe::RealtimeContext>(handle);
    if (!context) return;
    std::lock_guard<std::mutex> lock(context->stateMutex);
    if (alcybe::load32(&context->state) == alcybe::threadPaused)
        alcybe::store32(&context->state, alcybe::threadRunning);
    context->stateChanged.notify_all();
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_stopThread(JNIEnv*, jobject, jlong handle) {
    auto* context = alcybe::fromHandle<alcybe::RealtimeContext>(handle);
    if (!context) return;
    std::lock_guard<std::mutex> lock(context->stateMutex);
    alcybe::store32(&context->state, alcybe::threadStopped);
    context->stateChanged.notify_all();
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_joinThread(JNIEnv* env, jobject, jlong handle) {
    auto* context = alcybe::fromHandle<alcybe::RealtimeContext>(handle);
    if (!context || !context->worker.joinable()) return;
    if (std::this_thread::get_id() == context->worker.get_id()) {
        alcybe::throwJava(env, "java/lang/IllegalStateException", "Cannot join from the worker callback"); return;
    }
    context->worker.join();
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_destroyContext(JNIEnv* env, jobject instance, jlong handle) {
    auto* context = alcybe::fromHandle<alcybe::RealtimeContext>(handle);
    if (!context) return;
    Java_alcybe_realtime_RealtimeThread_stopThread(env, instance, handle);
    Java_alcybe_realtime_RealtimeThread_joinThread(env, instance, handle);
    if (env->ExceptionCheck()) return;
    env->DeleteGlobalRef(context->javaInstance);
    if (context->numaAllocation) alcybe::releaseOnNuma(context->sharedBlock);
    else alcybe::releaseAligned(context->sharedBlock);
    delete context;
}
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_currentTimeNanos(JNIEnv*, jclass) { return alcybe::nowNs(); }
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_currentTimeMicros(JNIEnv*, jclass) { return alcybe::nowNs() / 1000; }
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_currentTimeMillis(JNIEnv*, jclass) { return alcybe::nowNs() / 1000000; }
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_sleepNanos(JNIEnv*, jclass, jlong durationNs) {
    if (durationNs > 0) std::this_thread::sleep_for(std::chrono::nanoseconds(durationNs));
}
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_initMutex(JNIEnv*, jclass) {
    return static_cast<jlong>(reinterpret_cast<intptr_t>(new std::recursive_mutex()));
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_lockMutex(JNIEnv* env, jclass, jlong handle) {
    if (!handle) { alcybe::throwJava(env, "java/lang/IllegalStateException", "Mutex is closed"); return; }
    alcybe::fromHandle<std::recursive_mutex>(handle)->lock();
}
JNIEXPORT jboolean JNICALL Java_alcybe_realtime_RealtimeThread_tryLockMutex(JNIEnv* env, jclass, jlong handle) {
    if (!handle) { alcybe::throwJava(env, "java/lang/IllegalStateException", "Mutex is closed"); return JNI_FALSE; }
    return alcybe::fromHandle<std::recursive_mutex>(handle)->try_lock() ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_unlockMutex(JNIEnv* env, jclass, jlong handle) {
    if (!handle) { alcybe::throwJava(env, "java/lang/IllegalStateException", "Mutex is closed"); return; }
    alcybe::fromHandle<std::recursive_mutex>(handle)->unlock();
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_destroyMutex(JNIEnv*, jclass, jlong handle) {
    delete alcybe::fromHandle<std::recursive_mutex>(handle);
}
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_initBarrier(JNIEnv* env, jclass, jint parties) {
    if (parties <= 0) { alcybe::throwJava(env, "java/lang/IllegalArgumentException", "Barrier requires at least one party"); return 0; }
    return static_cast<jlong>(reinterpret_cast<intptr_t>(new alcybe::NativeBarrier(parties)));
}
JNIEXPORT jint JNICALL Java_alcybe_realtime_RealtimeThread_awaitBarrier(JNIEnv* env, jclass, jlong handle) {
    auto* barrier = alcybe::fromHandle<alcybe::NativeBarrier>(handle);
    if (!barrier) { alcybe::throwJava(env, "java/lang/IllegalStateException", "Barrier is closed"); return -1; }
    std::unique_lock<std::mutex> lock(barrier->mutex);
    int generation = barrier->generation;
    if (++barrier->arrived == barrier->members) {
        barrier->arrived = 0;
        ++barrier->generation;
        barrier->changed.notify_all();
        return 1;
    }
    barrier->changed.wait(lock, [&] { return generation != barrier->generation; });
    return 0;
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_destroyBarrier(JNIEnv*, jclass, jlong handle) {
    delete alcybe::fromHandle<alcybe::NativeBarrier>(handle);
}
}
