#include "camera_source.hpp"
#include "shared_camera.hpp"
#include "platform_support.hpp"
#include "jni_support.hpp"
#include <jni.h>
#include <algorithm>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <limits>
#include <system_error>

namespace alcybe {
extern JavaVM* javaVm;
struct CameraContext {
    std::unique_ptr<CameraSource> source;
    std::mutex captureMutex;
    std::mutex stateMutex;
    std::condition_variable stateChanged;
    std::thread worker;
    SharedCameraFrame* sharedBlock = nullptr;
    size_t sharedBytes = 0;
    bool numaAllocation = false;
    jobject javaInstance = nullptr;
    jmethodID frameMethod = nullptr;
    jmethodID gcMethod = nullptr;
    int width = 0;
    int height = 0;
    int targetFps = 0;
    int priority = 0;
    int cpuAffinity = -1;
    int64_t startNs = 0;
    int64_t offsetNs = 0;
    int64_t periodNs = 0;
    int64_t lastFrameNs = 0;
    int32_t state = cameraStopped;
};

bool readFrame(CameraContext* context, int timeoutMs) {
    std::lock_guard<std::mutex> lock(context->captureMutex);
    const int64_t arrivalNs = nowNs();
    int64_t timestampNs = 0;
    const bool received = context->source->readFrame(
        reinterpret_cast<uint32_t*>(context->sharedBlock->payload), timeoutMs, timestampNs);
    if (!received) {
        add32(&context->sharedBlock->droppedFrames, 1);
        return false;
    }
    if (timestampNs <= 0) timestampNs = nowNs();
    store64(&context->sharedBlock->frameIntervalNs,
        context->lastFrameNs > 0 ? timestampNs - context->lastFrameNs : 0);
    context->lastFrameNs = timestampNs;
    store64(&context->sharedBlock->hwTimestampNs, timestampNs);
    store64(&context->sharedBlock->pollTimestampNs, arrivalNs);
    store32(&context->sharedBlock->payloadBytesUsed, context->width * context->height * 4);
    add64(&context->sharedBlock->frameSequence, 1);
    return true;
}

void captureWorker(CameraContext* context) {
    JNIEnv* env = nullptr;
    if (javaVm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), nullptr) != JNI_OK) {
        store32(&context->state, cameraStopped);
        store32(&context->sharedBlock->cameraState, cameraStopped);
        return;
    }
    configureThread(context->priority, context->cpuAffinity);
    int64_t nextNs = std::max(context->startNs + context->offsetNs, nowNs());
    std::unique_lock<std::mutex> lock(context->stateMutex);
    while (load32(&context->state) != cameraStopped) {
        if (load32(&context->state) == cameraPaused) {
            store32(&context->sharedBlock->cameraState, cameraPaused);
            context->stateChanged.wait(lock, [&]() { return load32(&context->state) != cameraPaused; });
            nextNs = nowNs() + context->periodNs;
        }
        if (load32(&context->state) == cameraStopped) break;
        if (nowNs() < nextNs) {
            context->stateChanged.wait_until(lock,
                std::chrono::steady_clock::time_point(std::chrono::nanoseconds(nextNs)),
                [&]() { return load32(&context->state) != cameraRunning; });
            continue;
        }
        lock.unlock();
        store32(&context->sharedBlock->cameraState, cameraRunning);
        const int64_t beginNs = nowNs();
        // Capture can wait for a frame. This wait is part of the duty cycle.
        const int timeoutMs = static_cast<int>(std::clamp(context->periodNs / 1000000LL, 1LL, 1000LL));
        const bool received = readFrame(context, timeoutMs);
        int64_t endNs = nowNs();
        int64_t upcomingNs = nextNs + context->periodNs;
        int64_t idleNs = std::max<int64_t>(0, upcomingNs - endNs);
        store64(&context->sharedBlock->dutyCycleNs, endNs - beginNs);
        store64(&context->sharedBlock->idleCycleNs, idleNs);
        bool succeeded = true;
        if (received) {
            env->CallVoidMethod(context->javaInstance, context->frameMethod,
                static_cast<jlong>(context->sharedBlock->frameSequence),
                static_cast<jlong>(context->sharedBlock->hwTimestampNs),
                static_cast<jint>(context->sharedBlock->payloadBytesUsed));
            if (env->ExceptionCheck()) { clearException(env); succeeded = false; }
            if (succeeded && idleNs > gcIdleThresholdNs) {
                env->CallVoidMethod(context->javaInstance, context->gcMethod, static_cast<jlong>(idleNs));
                if (env->ExceptionCheck()) { clearException(env); succeeded = false; }
            }
        }
        endNs = nowNs();
        nextNs = upcomingNs;
        if (nextNs <= endNs)
            nextNs += ((endNs - nextNs) / context->periodNs + 1) * context->periodNs;
        lock.lock();
        if (!succeeded) store32(&context->state, cameraStopped);
    }
    store32(&context->state, cameraStopped);
    store32(&context->sharedBlock->cameraState, cameraStopped);
    lock.unlock();
    javaVm->DetachCurrentThread();
}
}

extern "C" {
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeCamera_openNativeCamera
(JNIEnv* env, jobject instance, jstring devicePath, jint width, jint height, jint fps, jint pixelMode, jint numaNode) {
    using namespace alcybe;
    if (!devicePath || width <= 0 || width > 8192 || height <= 0 || height > 8192 || fps <= 0 || fps > 1000
        || pixelMode < 0 || pixelMode > 1 || numaNode < -1 || (width & 1)) {
        throwJava(env, "java/lang/IllegalArgumentException", "Invalid camera dimensions or mode"); return 0;
    }
    if (pixelMode == 1) {
        throwJava(env, "java/lang/UnsupportedOperationException", "Raw MJPEG is only implemented in the Linux V4L2 backend"); return 0;
    }
    if (numaNode >= 0 && !numaNodeAvailable(numaNode)) {
        throwJava(env, "java/lang/UnsupportedOperationException", "Requested camera NUMA node is unavailable"); return 0;
    }
    const char* utf8 = env->GetStringUTFChars(devicePath, nullptr);
    if (!utf8) return 0;
    const std::string cameraId(utf8);
    env->ReleaseStringUTFChars(devicePath, utf8);
    if (cameraId.empty()) { throwJava(env, "java/lang/IllegalArgumentException", "Camera ID is empty"); return 0; }
    auto context = std::make_unique<CameraContext>();
    std::string errorMessage;
    context->source = openCameraSource(cameraId, width, height, fps, errorMessage);
    if (!context->source) {
        throwJava(env, "java/lang/IllegalStateException", errorMessage.empty() ? "Cannot open camera" : errorMessage.c_str());
        return 0;
    }
    const size_t pixelBytes = size_t(width) * size_t(height) * 4u;
    context->sharedBytes = (64u + pixelBytes + 63u) & ~size_t(63u);
    context->numaAllocation = numaNode >= 0;
    context->sharedBlock = static_cast<SharedCameraFrame*>(context->numaAllocation
        ? allocateOnNuma(context->sharedBytes, numaNode) : allocateAligned(context->sharedBytes));
    if (!context->sharedBlock) { throwJava(env, "java/lang/OutOfMemoryError", "Camera buffer allocation failed"); return 0; }
    std::memset(context->sharedBlock, 0, context->sharedBytes);
    context->javaInstance = env->NewGlobalRef(instance);
    jclass classRef = env->GetObjectClass(instance);
    context->frameMethod = classRef ? env->GetMethodID(classRef, "onNativeFrameReady", "(JJI)V") : nullptr;
    context->gcMethod = classRef && !env->ExceptionCheck() ? env->GetMethodID(classRef, "onNativeIdleGcWindow", "(J)V") : nullptr;
    if (classRef) env->DeleteLocalRef(classRef);
    if (env->ExceptionCheck() || !context->javaInstance || !context->frameMethod || !context->gcMethod) {
        if (context->javaInstance) env->DeleteGlobalRef(context->javaInstance);
        if (context->numaAllocation) releaseOnNuma(context->sharedBlock);
        else releaseAligned(context->sharedBlock);
        return 0;
    }
    context->width = width;
    context->height = height;
    context->targetFps = fps;
    return static_cast<jlong>(reinterpret_cast<intptr_t>(context.release()));
}
JNIEXPORT jobject JNICALL Java_alcybe_realtime_RealtimeCamera_getSharedFrameBuffer(JNIEnv* env, jobject, jlong handle) {
    auto* context = alcybe::fromHandle<alcybe::CameraContext>(handle);
    return context ? env->NewDirectByteBuffer(context->sharedBlock, static_cast<jlong>(context->sharedBytes)) : nullptr;
}
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeCamera_getSharedFrameAddress(JNIEnv*, jobject, jlong handle) {
    auto* context = alcybe::fromHandle<alcybe::CameraContext>(handle);
    return context ? static_cast<jlong>(reinterpret_cast<intptr_t>(context->sharedBlock)) : 0;
}
JNIEXPORT jint JNICALL Java_alcybe_realtime_RealtimeCamera_captureSingleFrame
(JNIEnv* env, jobject, jlong handle, jint timeoutMs, jboolean triggerGcAfter) {
    auto* context = alcybe::fromHandle<alcybe::CameraContext>(handle);
    if (!context) return -1;
    if (context->worker.joinable()) {
        alcybe::throwJava(env, "java/lang/IllegalStateException", "Stop scheduled capture before taking a single frame"); return -1;
    }
    const int64_t beginNs = alcybe::nowNs();
    const bool received = alcybe::readFrame(context, std::clamp(static_cast<int>(timeoutMs), 0, 60000));
    const int64_t durationNs = alcybe::nowNs() - beginNs;
    const int64_t idleNs = std::max<int64_t>(0, 1000000000LL / context->targetFps - durationNs);
    alcybe::store64(&context->sharedBlock->dutyCycleNs, durationNs);
    alcybe::store64(&context->sharedBlock->idleCycleNs, idleNs);
    if (received && triggerGcAfter == JNI_TRUE && idleNs > alcybe::gcIdleThresholdNs)
        env->CallVoidMethod(context->javaInstance, context->gcMethod, static_cast<jlong>(idleNs));
    return received ? static_cast<jint>(context->width * context->height * 4) : -1;
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeCamera_startScheduledCapture
(JNIEnv* env, jobject, jlong handle, jint priority, jint cpuAffinity, jlong startNs, jlong offsetNs, jlong periodNs) {
    auto* context = alcybe::fromHandle<alcybe::CameraContext>(handle);
    if (!context) return;
    if (priority < 0 || priority > 99 || cpuAffinity < -1 || offsetNs < 0 || offsetNs >= periodNs
        || periodNs <= 0 || startNs < 0 || startNs > INT64_MAX - offsetNs) {
        alcybe::throwJava(env, "java/lang/IllegalArgumentException", "Invalid priority, affinity or period"); return;
    }
#ifdef __APPLE__
    if (cpuAffinity >= 0) { alcybe::throwJava(env, "java/lang/UnsupportedOperationException", "macOS does not support CPU pinning"); return; }
#else
    if (cpuAffinity >= static_cast<int>(sizeof(uintptr_t) * 8)) { alcybe::throwJava(env, "java/lang/IllegalArgumentException", "CPU index is outside the current processor group"); return; }
#endif
    if (context->worker.joinable()) {
        if (alcybe::load32(&context->state) != alcybe::cameraStopped) {
            alcybe::throwJava(env, "java/lang/IllegalStateException", "Camera scheduler already active"); return;
        }
        if (context->worker.get_id() == std::this_thread::get_id()) {
            alcybe::throwJava(env, "java/lang/IllegalStateException", "Cannot restart from capture callback"); return;
        }
        context->worker.join();
    }
    context->priority = priority;
    context->cpuAffinity = cpuAffinity;
    context->startNs = startNs ? startNs : alcybe::nowNs();
    context->offsetNs = offsetNs;
    context->periodNs = periodNs;
    alcybe::store32(&context->state, alcybe::cameraRunning);
    try { context->worker = std::thread(alcybe::captureWorker, context); }
    catch (const std::system_error&) {
        alcybe::store32(&context->state, alcybe::cameraStopped);
        alcybe::throwJava(env, "java/lang/IllegalStateException", "Could not start capture worker");
    }
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeCamera_pauseCapture(JNIEnv*, jobject, jlong handle) {
    auto* context = alcybe::fromHandle<alcybe::CameraContext>(handle);
    if (!context) return;
    std::lock_guard<std::mutex> lock(context->stateMutex);
    if (alcybe::load32(&context->state) == alcybe::cameraRunning)
        alcybe::store32(&context->state, alcybe::cameraPaused);
    context->stateChanged.notify_all();
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeCamera_resumeCapture(JNIEnv*, jobject, jlong handle) {
    auto* context = alcybe::fromHandle<alcybe::CameraContext>(handle);
    if (!context) return;
    std::lock_guard<std::mutex> lock(context->stateMutex);
    if (alcybe::load32(&context->state) == alcybe::cameraPaused)
        alcybe::store32(&context->state, alcybe::cameraRunning);
    context->stateChanged.notify_all();
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeCamera_stopCapture(JNIEnv*, jobject, jlong handle) {
    auto* context = alcybe::fromHandle<alcybe::CameraContext>(handle);
    if (!context) return;
    std::lock_guard<std::mutex> lock(context->stateMutex);
    alcybe::store32(&context->state, alcybe::cameraStopped);
    context->stateChanged.notify_all();
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeCamera_closeNativeCamera(JNIEnv* env, jobject instance, jlong handle) {
    auto* context = alcybe::fromHandle<alcybe::CameraContext>(handle);
    if (!context) return;
    Java_alcybe_realtime_RealtimeCamera_stopCapture(env, instance, handle);
    if (context->worker.joinable()) {
        if (context->worker.get_id() == std::this_thread::get_id()) {
            alcybe::throwJava(env, "java/lang/IllegalStateException", "Cannot close from a capture callback"); return;
        }
        context->worker.join();
    }
    env->DeleteGlobalRef(context->javaInstance);
    if (context->numaAllocation) alcybe::releaseOnNuma(context->sharedBlock);
    else alcybe::releaseAligned(context->sharedBlock);
    delete context;
}
JNIEXPORT jboolean JNICALL Java_alcybe_realtime_RealtimeCamera_isV4L2ControlSupported(JNIEnv*, jobject, jlong, jint) { return JNI_FALSE; }
JNIEXPORT jboolean JNICALL Java_alcybe_realtime_RealtimeCamera_isV4L2MenuValueSupported(JNIEnv*, jobject, jlong, jint, jint) { return JNI_FALSE; }
JNIEXPORT jboolean JNICALL Java_alcybe_realtime_RealtimeCamera_setV4L2Control(JNIEnv*, jobject, jlong, jint, jint) { return JNI_FALSE; }
JNIEXPORT jint JNICALL Java_alcybe_realtime_RealtimeCamera_getV4L2Control(JNIEnv*, jobject, jlong, jint) { return -1; }
JNIEXPORT jboolean JNICALL Java_alcybe_realtime_RealtimeCamera_queryV4L2ControlRange(JNIEnv*, jobject, jlong, jint, jintArray) { return JNI_FALSE; }
}
