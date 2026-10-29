#include "alcybe_realtime_RealtimeCamera.h"
#include "camera_pixel.hpp"
#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sched.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#ifdef ALCYBE_WITH_NUMA
#include <numa.h>
#endif

namespace {
constexpr int64_t BILLION = 1000000000LL;
constexpr int64_t GC_IDLE_THRESHOLD_NS = 1000000LL;
constexpr size_t MAX_FRAME_BYTES = 512u * 1024u * 1024u;
JavaVM* cameraVm = nullptr;
int64_t monotonicNs() { timespec t{}; clock_gettime(CLOCK_MONOTONIC, &t); return int64_t(t.tv_sec) * BILLION + t.tv_nsec; }
int xioctl(int fd, unsigned long request, void* arg) {
    int rc;
    do { rc = ioctl(fd, request, arg); } while (rc == -1 && errno == EINTR);
    return rc;
}
void throwJava(JNIEnv* env, const char* type, const char* message) {
    jclass cls = env->FindClass(type);
    if (cls) { env->ThrowNew(cls, message); env->DeleteLocalRef(cls); }
}
void store32(int32_t* p, int32_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
void store64(int64_t* p, int64_t v) { __atomic_store_n(p, v, __ATOMIC_RELEASE); }
int32_t load32(const int32_t* p) { return __atomic_load_n(p, __ATOMIC_ACQUIRE); }
RealtimeCameraContext* camera(jlong ptr) { return reinterpret_cast<RealtimeCameraContext*>(static_cast<intptr_t>(ptr)); }

void releaseCamera(JNIEnv* env, RealtimeCameraContext* ctx) {
    if (!ctx) return;
    if (ctx->streamOn) {
        v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(ctx->fd, VIDIOC_STREAMOFF, &type);
    }
    if (ctx->kernelBuffers) {
        for (unsigned int i = 0; i < ctx->bufferCount; ++i) {
            if (ctx->kernelBuffers[i].start != MAP_FAILED && ctx->kernelBuffers[i].start)
                munmap(ctx->kernelBuffers[i].start, ctx->kernelBuffers[i].length);
        }
        free(ctx->kernelBuffers);
    }
    if (ctx->fd >= 0) close(ctx->fd);
    if (ctx->javaCameraInstance) env->DeleteGlobalRef(ctx->javaCameraInstance);
    pthread_cond_destroy(&ctx->stateCond);
    pthread_mutex_destroy(&ctx->captureMutex);
    pthread_mutex_destroy(&ctx->stateMutex);
    if (ctx->sharedBlock) {
#ifdef ALCYBE_WITH_NUMA
        if (ctx->numaSharedAllocation) numa_free(ctx->sharedBlock, ctx->sharedBlockTotalSize);
        else
#endif
        free(ctx->sharedBlock);
    }
    free(ctx);
}

void configureHardware(const RealtimeCameraContext* ctx) {
    if (ctx->cpuAffinity >= 0 && ctx->cpuAffinity < CPU_SETSIZE) {
        cpu_set_t cpus; CPU_ZERO(&cpus); CPU_SET(ctx->cpuAffinity, &cpus);
        int rc = pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
        if (rc) fprintf(stderr, "[camera] affinity unavailable: %s\n", strerror(rc));
    }
    if (ctx->priority > 0) {
        sched_param param{}; param.sched_priority = ctx->priority;
        int rc = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
        if (rc) fprintf(stderr, "[camera] SCHED_FIFO unavailable: %s (continuing without RT priority)\n", strerror(rc));
    }
}

// Must be called only with captureMutex held. One producer owns the shared frame buffer.
bool dequeueLatestFrame(RealtimeCameraContext* ctx, int64_t pollWakeNs, int timeoutMs) {
    pollfd pfd{ctx->fd, POLLIN, 0};
    int ready;
    do { ready = poll(&pfd, 1, timeoutMs); } while (ready < 0 && errno == EINTR);
    if (ready <= 0 || !(pfd.revents & POLLIN)) {
        __atomic_add_fetch(&ctx->sharedBlock->droppedFrames, int32_t(1), __ATOMIC_RELAXED);
        return false;
    }
    v4l2_buffer last{};
    last.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; last.memory = V4L2_MEMORY_MMAP;
    if (xioctl(ctx->fd, VIDIOC_DQBUF, &last) < 0) return false;
    // Newest frame wins: return older frames immediately to the driver.
    v4l2_buffer next{};
    for (;;) {
        next = {};
        next.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; next.memory = V4L2_MEMORY_MMAP;
        if (xioctl(ctx->fd, VIDIOC_DQBUF, &next) < 0) break;
        xioctl(ctx->fd, VIDIOC_QBUF, &last);
        last = next;
        __atomic_add_fetch(&ctx->sharedBlock->droppedFrames, int32_t(1), __ATOMIC_RELAXED);
    }
    bool ok = false;
    if (last.index < ctx->bufferCount && ctx->kernelBuffers[last.index].start != MAP_FAILED
            && last.bytesused <= ctx->kernelBuffers[last.index].length && last.bytesused > 0) {
        const auto* source = static_cast<const uint8_t*>(ctx->kernelBuffers[last.index].start);
        if (ctx->pixelMode == PIXEL_MODE_YUYV_TO_INT_RGB) {
            const size_t needed = (static_cast<size_t>(ctx->height) - 1u) * ctx->rowStride
                                + static_cast<size_t>(ctx->width) * 2u;
            if (last.bytesused >= needed) {
                alcybe_camera::yuyvToArgb(source, ctx->rowStride,
                    reinterpret_cast<uint32_t*>(ctx->sharedBlock->payload), ctx->width, ctx->height);
                store32(&ctx->sharedBlock->payloadBytesUsed, ctx->width * ctx->height * 4);
                ok = true;
            }
        } else if (last.bytesused <= ctx->payloadCapacity) {
            memcpy(ctx->sharedBlock->payload, source, last.bytesused);
            store32(&ctx->sharedBlock->payloadBytesUsed, static_cast<int32_t>(last.bytesused));
            ok = true;
        }
        if (ok) {
            const int64_t timeNs = int64_t(last.timestamp.tv_sec) * BILLION
                                   + int64_t(last.timestamp.tv_usec) * 1000LL;
            store64(&ctx->sharedBlock->frameIntervalNs, ctx->lastFrameHwNs > 0 ? timeNs - ctx->lastFrameHwNs : 0);
            ctx->lastFrameHwNs = timeNs;
            store64(&ctx->sharedBlock->hwTimestampNs, timeNs);
            store64(&ctx->sharedBlock->pollTimestampNs, pollWakeNs);
            __atomic_add_fetch(&ctx->sharedBlock->frameSequence, int64_t(1), __ATOMIC_RELEASE);
        }
    }
    if (!ok) __atomic_add_fetch(&ctx->sharedBlock->droppedFrames, int32_t(1), __ATOMIC_RELAXED);
    if (xioctl(ctx->fd, VIDIOC_QBUF, &last) < 0) fprintf(stderr, "[camera] VIDIOC_QBUF failed\n");
    return ok;
}

bool queryWritable(int fd, uint32_t id, v4l2_queryctrl* result, bool allowInactive = false) {
    v4l2_queryctrl q{}; q.id = id;
    if (xioctl(fd, VIDIOC_QUERYCTRL, &q) < 0) return false;
    if ((q.flags & (V4L2_CTRL_FLAG_DISABLED | V4L2_CTRL_FLAG_READ_ONLY)) ||
        (!allowInactive && (q.flags & V4L2_CTRL_FLAG_INACTIVE))) return false;
    if (result) *result = q;
    return true;
}
bool menuSupported(int fd, uint32_t id, int value) {
    if (value < 0) return false;
    v4l2_querymenu menu{}; menu.id = id; menu.index = static_cast<uint32_t>(value);
    return xioctl(fd, VIDIOC_QUERYMENU, &menu) == 0;
}
bool validatedSet(int fd, uint32_t id, int value) {
    v4l2_queryctrl q{};
    if (!queryWritable(fd, id, &q)) return false;
    if (q.type == V4L2_CTRL_TYPE_MENU || q.type == V4L2_CTRL_TYPE_INTEGER_MENU) {
        if (value < q.minimum || value > q.maximum || !menuSupported(fd, id, value)) return false;
    } else if (q.type == V4L2_CTRL_TYPE_BOOLEAN) {
        value = value ? 1 : 0;
    } else if (q.type == V4L2_CTRL_TYPE_INTEGER) {
        value = std::clamp(value, q.minimum, q.maximum);
        if (q.step > 1) value = q.minimum + (value - q.minimum) / q.step * q.step;
    } else return false;
    v4l2_control c{}; c.id = id; c.value = value;
    return xioctl(fd, VIDIOC_S_CTRL, &c) == 0;
}

void* schedulerMain(void* argument) {
    auto* ctx = static_cast<RealtimeCameraContext*>(argument);
    JNIEnv* env = nullptr;
    JavaVMAttachArgs args{JNI_VERSION_1_8, const_cast<char*>("Alcybe-Camera"), nullptr};
    if (cameraVm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK) {
        store32(&ctx->state, CAM_STATE_STOPPED);
        store32(&ctx->sharedBlock->cameraState, CAM_STATE_STOPPED);
        return nullptr;
    }
    configureHardware(ctx);
    int64_t next = ctx->beginTimeNs + ctx->offsetNs;
    const int64_t now = monotonicNs();
    if (next < now) next = now;
    const int timeoutMs = static_cast<int>(std::clamp(ctx->cycleTimeNs / 1000000LL, 1LL, 1000LL));
    for (;;) {
        pthread_mutex_lock(&ctx->stateMutex);
        while (load32(&ctx->state) == CAM_STATE_PAUSED) {
            store32(&ctx->sharedBlock->cameraState, CAM_STATE_PAUSED);
            pthread_cond_wait(&ctx->stateCond, &ctx->stateMutex);
            next = monotonicNs() + ctx->cycleTimeNs;
        }
        if (load32(&ctx->state) == CAM_STATE_STOPPED) {
            pthread_mutex_unlock(&ctx->stateMutex);
            break;
        }
        pthread_mutex_unlock(&ctx->stateMutex);
        store32(&ctx->sharedBlock->cameraState, CAM_STATE_RUNNING);
        // Timed sleep is interruptible by stop/pause signals (unlike clock_nanosleep).
        pthread_mutex_lock(&ctx->stateMutex);
        while (load32(&ctx->state) == CAM_STATE_RUNNING && monotonicNs() < next) {
            timespec deadline{time_t(next / BILLION), long(next % BILLION)};
            int rc = pthread_cond_timedwait(&ctx->stateCond, &ctx->stateMutex, &deadline);
            if (rc != 0 && rc != ETIMEDOUT) store32(&ctx->state, CAM_STATE_STOPPED);
        }
        bool running = load32(&ctx->state) == CAM_STATE_RUNNING;
        pthread_mutex_unlock(&ctx->stateMutex);
        if (!running) continue;
        const int64_t start = monotonicNs();
        pthread_mutex_lock(&ctx->captureMutex);
        const bool gotFrame = dequeueLatestFrame(ctx, start, timeoutMs);
        pthread_mutex_unlock(&ctx->captureMutex);
        if (gotFrame) {
            env->CallVoidMethod(ctx->javaCameraInstance, ctx->onFrameMethodId,
                                static_cast<jlong>(__atomic_load_n(&ctx->sharedBlock->frameSequence, __ATOMIC_ACQUIRE)),
                                static_cast<jlong>(__atomic_load_n(&ctx->sharedBlock->hwTimestampNs, __ATOMIC_ACQUIRE)),
                                static_cast<jint>(load32(&ctx->sharedBlock->payloadBytesUsed)));
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe(); env->ExceptionClear();
                store32(&ctx->state, CAM_STATE_STOPPED);
            }
        }
        const int64_t end = monotonicNs();
        store64(&ctx->sharedBlock->dutyCycleNs, end - start);
        next += ctx->cycleTimeNs;
        const int64_t idle = std::max<int64_t>(0, next - end);
        store64(&ctx->sharedBlock->idleCycleNs, idle);
        if (load32(&ctx->state) == CAM_STATE_RUNNING && idle > GC_IDLE_THRESHOLD_NS) {
            env->CallVoidMethod(ctx->javaCameraInstance, ctx->onIdleGcMethodId, static_cast<jlong>(idle));
            if (env->ExceptionCheck()) {
                env->ExceptionDescribe(); env->ExceptionClear();
                store32(&ctx->state, CAM_STATE_STOPPED);
            }
        }
        if (next <= end) next += ((end - next) / ctx->cycleTimeNs + 1) * ctx->cycleTimeNs;
    }
    store32(&ctx->state, CAM_STATE_STOPPED);
    store32(&ctx->sharedBlock->cameraState, CAM_STATE_STOPPED);
    cameraVm->DetachCurrentThread();
    return nullptr;
}
} // namespace

extern "C" {
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeCamera_openNativeCamera
(JNIEnv* env, jobject obj, jstring devicePath, jint width, jint height, jint targetFps, jint pixelMode, jint numaNode) {
    if (!devicePath || width <= 0 || height <= 0 || (width & 1) != 0 || targetFps <= 0 ||
        (pixelMode != PIXEL_MODE_RAW_MJPEG && pixelMode != PIXEL_MODE_YUYV_TO_INT_RGB) || numaNode < -1 ||
        static_cast<int64_t>(width) * height * 4 > static_cast<int64_t>(MAX_FRAME_BYTES)) {
        throwJava(env, "java/lang/IllegalArgumentException", "Invalid camera dimensions (even width required), format, FPS or NUMA node");
        return 0;
    }
#ifndef ALCYBE_WITH_NUMA
    if (numaNode >= 0) {
        throwJava(env, "java/lang/UnsupportedOperationException", "NUMA support not compiled; install libnuma-dev and rebuild");
        return 0;
    }
#else
    if (numaNode >= 0 && (numa_available() < 0 || numaNode > numa_max_node())) {
        throwJava(env, "java/lang/IllegalArgumentException", "NUMA node unavailable");
        return 0;
    }
#endif
    env->GetJavaVM(&cameraVm);
    const char* name = env->GetStringUTFChars(devicePath, nullptr);
    if (!name) return 0;
    int fd = open(name, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    env->ReleaseStringUTFChars(devicePath, name);
    if (fd < 0) return 0;
    auto* ctx = static_cast<RealtimeCameraContext*>(nullptr);
    if (posix_memalign(reinterpret_cast<void**>(&ctx), 64, sizeof(RealtimeCameraContext)) != 0) {
        close(fd); return 0;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = fd;
    ctx->width = width; ctx->height = height; ctx->targetFps = targetFps;
    ctx->pixelMode = pixelMode; ctx->numaNode = numaNode;
    // All synchronization objects are initialized before any error path can release ctx.
    pthread_mutexattr_t mutexAttr;
    pthread_mutexattr_init(&mutexAttr);
    pthread_mutexattr_setprotocol(&mutexAttr, PTHREAD_PRIO_INHERIT);
    pthread_mutex_init(&ctx->stateMutex, &mutexAttr);
    pthread_mutex_init(&ctx->captureMutex, &mutexAttr);
    pthread_mutexattr_destroy(&mutexAttr);
    pthread_condattr_t condAttr; pthread_condattr_init(&condAttr);
    pthread_condattr_setclock(&condAttr, CLOCK_MONOTONIC);
    pthread_cond_init(&ctx->stateCond, &condAttr);
    pthread_condattr_destroy(&condAttr);

    v4l2_capability caps{};
    if (xioctl(fd, VIDIOC_QUERYCAP, &caps) < 0) { releaseCamera(env, ctx); return 0; }
    const uint32_t activeCaps = (caps.capabilities & V4L2_CAP_DEVICE_CAPS) ? caps.device_caps : caps.capabilities;
    if (!(activeCaps & V4L2_CAP_VIDEO_CAPTURE) || !(activeCaps & V4L2_CAP_STREAMING)) {
        releaseCamera(env, ctx); return 0;
    }
    v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = width;
    fmt.fmt.pix.height = height;
    fmt.fmt.pix.pixelformat = pixelMode == PIXEL_MODE_RAW_MJPEG ? V4L2_PIX_FMT_MJPEG : V4L2_PIX_FMT_YUYV;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (xioctl(fd, VIDIOC_S_FMT, &fmt) < 0 || static_cast<int>(fmt.fmt.pix.width) != width ||
        static_cast<int>(fmt.fmt.pix.height) != height ||
        fmt.fmt.pix.pixelformat != (pixelMode == PIXEL_MODE_RAW_MJPEG ? V4L2_PIX_FMT_MJPEG : V4L2_PIX_FMT_YUYV)) {
        releaseCamera(env, ctx); return 0;
    }
    ctx->rowStride = fmt.fmt.pix.bytesperline;
    if (pixelMode == PIXEL_MODE_YUYV_TO_INT_RGB && ctx->rowStride < static_cast<size_t>(width) * 2u) {
        releaseCamera(env, ctx); return 0;
    }
    // MJPEG is variable length; reserve enough for the driver-advertised maximum.
    ctx->payloadCapacity = std::max(static_cast<size_t>(width) * height * 4u,
                                    static_cast<size_t>(fmt.fmt.pix.sizeimage));
    if (ctx->payloadCapacity == 0 || ctx->payloadCapacity > MAX_FRAME_BYTES) {
        releaseCamera(env, ctx); return 0;
    }
    ctx->sharedBlockTotalSize = (64u + ctx->payloadCapacity + 63u) & ~size_t(63u);
    void* block = nullptr;
#ifdef ALCYBE_WITH_NUMA
    if (numaNode >= 0) {
        block = numa_alloc_onnode(ctx->sharedBlockTotalSize, numaNode);
        if (block) ctx->numaSharedAllocation = true;
    } else
#endif
    {
        if (posix_memalign(&block, 64, ctx->sharedBlockTotalSize) != 0) block = nullptr;
    }
    if (!block) { releaseCamera(env, ctx); return 0; }
    ctx->sharedBlock = static_cast<SharedCameraFrameBlock*>(block);
    memset(ctx->sharedBlock, 0, ctx->sharedBlockTotalSize);
    v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = targetFps;
    if (xioctl(fd, VIDIOC_S_PARM, &parm) < 0) {
        fprintf(stderr, "[camera] VIDIOC_S_PARM unsupported; frame rate may differ\n");
    }
    v4l2_requestbuffers req{};
    req.count = 4; req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0 || req.count == 0) { releaseCamera(env, ctx); return 0; }
    ctx->bufferCount = req.count;
    ctx->kernelBuffers = static_cast<MmapBuffer*>(calloc(req.count, sizeof(MmapBuffer)));
    if (!ctx->kernelBuffers) { releaseCamera(env, ctx); return 0; }
    for (unsigned int i = 0; i < req.count; ++i) {
        ctx->kernelBuffers[i].start = MAP_FAILED;
        v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; buf.memory = V4L2_MEMORY_MMAP; buf.index = i;
        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) < 0 || buf.length == 0) { releaseCamera(env, ctx); return 0; }
        ctx->kernelBuffers[i].length = buf.length;
        ctx->kernelBuffers[i].start = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buf.m.offset);
        if (ctx->kernelBuffers[i].start == MAP_FAILED || xioctl(fd, VIDIOC_QBUF, &buf) < 0) {
            releaseCamera(env, ctx); return 0;
        }
    }
    v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) { releaseCamera(env, ctx); return 0; }
    ctx->streamOn = true;
    ctx->javaCameraInstance = env->NewGlobalRef(obj);
    jclass cls = env->GetObjectClass(obj);
    ctx->onFrameMethodId = cls ? env->GetMethodID(cls, "onNativeFrameReady", "(JJI)V") : nullptr;
    ctx->onIdleGcMethodId = cls && !env->ExceptionCheck() ? env->GetMethodID(cls, "onNativeIdleGcWindow", "(J)V") : nullptr;
    if (cls) env->DeleteLocalRef(cls);
    if (!ctx->javaCameraInstance || !ctx->onFrameMethodId || !ctx->onIdleGcMethodId || env->ExceptionCheck()) {
        releaseCamera(env, ctx); return 0;
    }
    store32(&ctx->state, CAM_STATE_STOPPED);
    return reinterpret_cast<jlong>(ctx);
}
JNIEXPORT jobject JNICALL Java_alcybe_realtime_RealtimeCamera_getSharedFrameBuffer
(JNIEnv* env, jobject, jlong ptr) {
    auto* ctx = camera(ptr);
    return ctx ? env->NewDirectByteBuffer(ctx->sharedBlock, static_cast<jlong>(ctx->sharedBlockTotalSize)) : nullptr;
}
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeCamera_getSharedFrameAddress
(JNIEnv*, jobject, jlong ptr) {
    auto* ctx = camera(ptr);
    return ctx ? reinterpret_cast<jlong>(ctx->sharedBlock) : 0;
}
JNIEXPORT jint JNICALL Java_alcybe_realtime_RealtimeCamera_captureSingleFrame
(JNIEnv* env, jobject, jlong ptr, jint timeoutMs, jboolean triggerGcAfter) {
    auto* ctx = camera(ptr);
    if (!ctx) return -1;
    if (ctx->schedulerStarted) {
        throwJava(env, "java/lang/IllegalStateException", "Synchronous capture cannot run concurrently with scheduled capture");
        return -1;
    }
    const int64_t start = monotonicNs();
    pthread_mutex_lock(&ctx->captureMutex);
    const bool ok = dequeueLatestFrame(ctx, start, std::clamp(static_cast<int>(timeoutMs), 0, 60000));
    pthread_mutex_unlock(&ctx->captureMutex);
    const int64_t end = monotonicNs();
    store64(&ctx->sharedBlock->dutyCycleNs, end - start);
    const int64_t idle = std::max<int64_t>(0, BILLION / ctx->targetFps - (end - start));
    store64(&ctx->sharedBlock->idleCycleNs, idle);
    if (ok && triggerGcAfter && idle > GC_IDLE_THRESHOLD_NS) {
        env->CallVoidMethod(ctx->javaCameraInstance, ctx->onIdleGcMethodId, static_cast<jlong>(idle));
    }
    return ok ? load32(&ctx->sharedBlock->payloadBytesUsed) : -1;
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeCamera_startScheduledCapture
(JNIEnv* env, jobject, jlong ptr, jint priority, jint cpuAffinity, jlong beginNs, jlong offsetNs, jlong cycleNs) {
    auto* ctx = camera(ptr);
    if (!ctx) return;
    if (priority < 0 || priority > 99 || cpuAffinity < -1 || cpuAffinity >= CPU_SETSIZE ||
        cycleNs <= 0 || offsetNs < 0 || offsetNs >= cycleNs || beginNs < 0) {
        throwJava(env, "java/lang/IllegalArgumentException", "Invalid scheduler priority, CPU, deadline or period");
        return;
    }
    if (ctx->schedulerStarted) {
        if (load32(&ctx->state) != CAM_STATE_STOPPED) {
            throwJava(env, "java/lang/IllegalStateException", "Camera scheduler already running"); return;
        }
        if (pthread_equal(ctx->schedulerThread, pthread_self())) {
            throwJava(env, "java/lang/IllegalStateException", "Cannot rejoin from own scheduler callback"); return;
        }
        pthread_join(ctx->schedulerThread, nullptr);
        ctx->schedulerStarted = false;
    }
    ctx->priority = priority; ctx->cpuAffinity = cpuAffinity;
    ctx->beginTimeNs = beginNs ? beginNs : monotonicNs();
    ctx->offsetNs = offsetNs; ctx->cycleTimeNs = cycleNs;
    store32(&ctx->state, CAM_STATE_RUNNING);
    int rc = pthread_create(&ctx->schedulerThread, nullptr, schedulerMain, ctx);
    if (rc) {
        store32(&ctx->state, CAM_STATE_STOPPED);
        throwJava(env, "java/lang/IllegalStateException", "pthread_create camera scheduler failed");
        return;
    }
    ctx->schedulerStarted = true;
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeCamera_pauseCapture
(JNIEnv*, jobject, jlong ptr) {
    auto* ctx = camera(ptr); if (!ctx) return;
    pthread_mutex_lock(&ctx->stateMutex);
    if (load32(&ctx->state) == CAM_STATE_RUNNING) store32(&ctx->state, CAM_STATE_PAUSED);
    pthread_cond_broadcast(&ctx->stateCond); pthread_mutex_unlock(&ctx->stateMutex);
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeCamera_resumeCapture
(JNIEnv*, jobject, jlong ptr) {
    auto* ctx = camera(ptr); if (!ctx) return;
    pthread_mutex_lock(&ctx->stateMutex);
    if (load32(&ctx->state) == CAM_STATE_PAUSED) store32(&ctx->state, CAM_STATE_RUNNING);
    pthread_cond_broadcast(&ctx->stateCond); pthread_mutex_unlock(&ctx->stateMutex);
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeCamera_stopCapture
(JNIEnv*, jobject, jlong ptr) {
    auto* ctx = camera(ptr); if (!ctx) return;
    pthread_mutex_lock(&ctx->stateMutex);
    store32(&ctx->state, CAM_STATE_STOPPED);
    pthread_cond_broadcast(&ctx->stateCond); pthread_mutex_unlock(&ctx->stateMutex);
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeCamera_closeNativeCamera
(JNIEnv* env, jobject, jlong ptr) {
    auto* ctx = camera(ptr); if (!ctx) return;
    pthread_mutex_lock(&ctx->stateMutex);
    store32(&ctx->state, CAM_STATE_STOPPED);
    pthread_cond_broadcast(&ctx->stateCond);
    pthread_mutex_unlock(&ctx->stateMutex);
    if (ctx->schedulerStarted) {
        if (pthread_equal(ctx->schedulerThread, pthread_self())) {
            throwJava(env, "java/lang/IllegalStateException", "close() cannot be called from inside the camera callback; close from another thread");
            return;
        }
        pthread_join(ctx->schedulerThread, nullptr);
    }
    releaseCamera(env, ctx);
}
JNIEXPORT jboolean JNICALL Java_alcybe_realtime_RealtimeCamera_isV4L2ControlSupported
(JNIEnv*, jobject, jlong ptr, jint id) {
    auto* ctx = camera(ptr);
    if (!ctx) return JNI_FALSE;
    return queryWritable(ctx->fd, static_cast<uint32_t>(id), nullptr) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL Java_alcybe_realtime_RealtimeCamera_isV4L2MenuValueSupported
(JNIEnv*, jobject, jlong ptr, jint id, jint value) {
    auto* ctx = camera(ptr); if (!ctx) return JNI_FALSE;
    v4l2_queryctrl q{};
    if (!queryWritable(ctx->fd, static_cast<uint32_t>(id), &q) ||
        (q.type != V4L2_CTRL_TYPE_MENU && q.type != V4L2_CTRL_TYPE_INTEGER_MENU)) return JNI_FALSE;
    return menuSupported(ctx->fd, static_cast<uint32_t>(id), value) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jboolean JNICALL Java_alcybe_realtime_RealtimeCamera_setV4L2Control
(JNIEnv*, jobject, jlong ptr, jint id, jint value) {
    auto* ctx = camera(ptr);
    return ctx && validatedSet(ctx->fd, static_cast<uint32_t>(id), value) ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT jint JNICALL Java_alcybe_realtime_RealtimeCamera_getV4L2Control
(JNIEnv*, jobject, jlong ptr, jint id) {
    auto* ctx = camera(ptr); if (!ctx) return -1;
    v4l2_queryctrl q{}; q.id = static_cast<uint32_t>(id);
    if (xioctl(ctx->fd, VIDIOC_QUERYCTRL, &q) < 0 || (q.flags & V4L2_CTRL_FLAG_DISABLED)) return -1;
    v4l2_control control{}; control.id = static_cast<uint32_t>(id);
    return xioctl(ctx->fd, VIDIOC_G_CTRL, &control) == 0 ? control.value : -1;
}
JNIEXPORT jboolean JNICALL Java_alcybe_realtime_RealtimeCamera_queryV4L2ControlRange
(JNIEnv* env, jobject, jlong ptr, jint id, jintArray out) {
    auto* ctx = camera(ptr);
    if (!ctx || !out || env->GetArrayLength(out) < 4) return JNI_FALSE;
    v4l2_queryctrl q{}; q.id = static_cast<uint32_t>(id);
    if (xioctl(ctx->fd, VIDIOC_QUERYCTRL, &q) < 0 || (q.flags & V4L2_CTRL_FLAG_DISABLED)) return JNI_FALSE;
    jint vals[5]{q.minimum, q.maximum, q.step, q.default_value, static_cast<jint>(q.flags)};
    env->SetIntArrayRegion(out, 0, env->GetArrayLength(out) >= 5 ? 5 : 4, vals);
    return JNI_TRUE;
}
} // extern "C"
