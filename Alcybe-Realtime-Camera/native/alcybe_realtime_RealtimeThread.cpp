#include "alcybe_realtime_RealtimeThread.h"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>
#include <sched.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#ifdef ALCYBE_WITH_NUMA
#include <numa.h>
#endif

static JavaVM* g_vm = nullptr;
constexpr int64_t BILLION = 1000000000LL;
constexpr int64_t GC_IDLE_THRESHOLD_NS = 1000000LL;

static void throwJava(JNIEnv* env, const char* type, const char* msg) {
    jclass clazz = env->FindClass(type);
    if (clazz) { env->ThrowNew(clazz, msg); env->DeleteLocalRef(clazz); }
}
static void throwErrno(JNIEnv* env, const char* operation, int error) {
    char msg[256];
    snprintf(msg, sizeof(msg), "%s: %s (%d)", operation, strerror(error), error);
    throwJava(env, "java/lang/IllegalStateException", msg);
}
static int64_t nowNanos() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * BILLION + ts.tv_nsec;
}
static timespec nanosToTime(int64_t ns) {
    return timespec{time_t(ns / BILLION), long(ns % BILLION)};
}
static void put64(int64_t* field, int64_t value) { __atomic_store_n(field, value, __ATOMIC_RELEASE); }
static void put32(int32_t* field, int32_t value) { __atomic_store_n(field, value, __ATOMIC_RELEASE); }
static int32_t get32(const int32_t* field) { return __atomic_load_n(field, __ATOMIC_ACQUIRE); }
static RealtimeContext* context(jlong ptr) { return reinterpret_cast<RealtimeContext*>(static_cast<intptr_t>(ptr)); }
static void warnError(const char* what, int error) {
    if (error) fprintf(stderr, "[alcybe-realtime] %s: %s (%d); requested RT setting not active\n", what, strerror(error), error);
}
static void configureHardware(RealtimeContext* ctx) {
    if (ctx->cpuAffinity >= 0) {
        cpu_set_t set;
        CPU_ZERO(&set);
        CPU_SET(ctx->cpuAffinity, &set);
        warnError("pthread_setaffinity_np", pthread_setaffinity_np(pthread_self(), sizeof(set), &set));
    }
    if (ctx->priority > 0) {
        sched_param param{};
        param.sched_priority = ctx->priority;
        warnError("pthread_setschedparam(SCHED_FIFO)", pthread_setschedparam(pthread_self(), SCHED_FIFO, &param));
        // Failed request leaves this thread in SCHED_OTHER, not silently SCHED_RR.
    }
}

static bool invokeCallback(RealtimeContext* ctx, JNIEnv* env) {
    env->CallVoidMethod(ctx->javaInstance, ctx->onCycleMethodId,
                        static_cast<jlong>(reinterpret_cast<intptr_t>(ctx->sharedBlock)));
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
        return false;
    }
    return true;
}
static void* workerMain(void* arg) {
    auto* ctx = static_cast<RealtimeContext*>(arg);
    JNIEnv* env = nullptr;
    JavaVMAttachArgs args{JNI_VERSION_1_8, const_cast<char*>("Alcybe-RT"), nullptr};
    if (g_vm->AttachCurrentThreadAsDaemon(reinterpret_cast<void**>(&env), &args) != JNI_OK) {
        put32(&ctx->state, STATE_STOPPED);
        put32(&ctx->sharedBlock->threadState, STATE_STOPPED);
        return nullptr;
    }
    configureHardware(ctx);
    if (ctx->mode == 1) {
        if (get32(&ctx->state) == STATE_RUNNING) {
            const int64_t start = nowNanos();
            bool ok = invokeCallback(ctx, env);
            put64(&ctx->sharedBlock->dutyCycleNs, nowNanos() - start);
            __atomic_add_fetch(&ctx->sharedBlock->sequenceNumber, int64_t(1), __ATOMIC_ACQ_REL);
            (void)ok;
        }
    } else {
        int64_t target = ctx->beginTimeNs + ctx->offsetNs;
        pthread_mutex_lock(&ctx->stateMutex);
        while (ctx->state != STATE_STOPPED) {
            if (ctx->state == STATE_PAUSED) {
                put32(&ctx->sharedBlock->threadState, STATE_PAUSED);
                while (ctx->state == STATE_PAUSED) pthread_cond_wait(&ctx->stateCond, &ctx->stateMutex);
                if (ctx->state == STATE_STOPPED) break;
                target = nowNanos() + ctx->cycleTimeNs;
            }
            while (ctx->state == STATE_RUNNING && nowNanos() < target) {
                timespec ts = nanosToTime(target);
                int rc = pthread_cond_timedwait(&ctx->stateCond, &ctx->stateMutex, &ts);
                if (rc != 0 && rc != ETIMEDOUT) {
                    fprintf(stderr, "[alcybe-realtime] timedwait error %d\n", rc);
                    put32(&ctx->state, STATE_STOPPED);
                }
            }
            if (ctx->state != STATE_RUNNING) continue;
            pthread_mutex_unlock(&ctx->stateMutex);
            put32(&ctx->sharedBlock->threadState, STATE_RUNNING);
            const int64_t start = nowNanos();
            put64(&ctx->sharedBlock->actualJitterNs, std::max<int64_t>(0, start - target));
            bool ok = invokeCallback(ctx, env);
            const int64_t end = nowNanos();
            const int64_t next = target + ctx->cycleTimeNs;
            const int64_t idle = std::max<int64_t>(0, next - end);
            put64(&ctx->sharedBlock->dutyCycleNs, end - start);
            put64(&ctx->sharedBlock->idleCycleNs, idle);
            __atomic_add_fetch(&ctx->sharedBlock->sequenceNumber, int64_t(1), __ATOMIC_ACQ_REL);
            if (ok && ctx->gcEnabled && idle > GC_IDLE_THRESHOLD_NS) {
                env->CallVoidMethod(ctx->javaInstance, ctx->requestGcMethodId, static_cast<jlong>(idle));
                if (env->ExceptionCheck()) {
                    env->ExceptionDescribe();
                    env->ExceptionClear();
                    ok = false;
                }
            }
            // Skip missed periods rather than running a backlog of late callbacks.
            target = next;
            if (target <= end) target += ((end - target) / ctx->cycleTimeNs + 1) * ctx->cycleTimeNs;
            pthread_mutex_lock(&ctx->stateMutex);
            if (!ok) put32(&ctx->state, STATE_STOPPED);
        }
        pthread_mutex_unlock(&ctx->stateMutex);
    }
    put32(&ctx->state, STATE_STOPPED);
    put32(&ctx->sharedBlock->threadState, STATE_STOPPED);
    g_vm->DetachCurrentThread();
    return nullptr;
}

extern "C" {
JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    return JNI_VERSION_1_8;
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_initSystem(JNIEnv*, jclass, jint) {
    // Deliberately do not change JVM main thread scheduling or mlockall at class load.
    // mlockall(MCL_FUTURE) without sufficient memlock limit can break later JVM allocations.
}
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_createContext
(JNIEnv* env, jobject obj, jint priority, jint cpuAffinity, jint numaNode, jint sharedMemSize, jboolean gcEnabled) {
    if (priority < 0 || priority > 99 || cpuAffinity < -1 || cpuAffinity >= CPU_SETSIZE || numaNode < -1 || sharedMemSize < 128 || sharedMemSize > 1 << 28) {
        throwJava(env, "java/lang/IllegalArgumentException", "Invalid RT priority, CPU, NUMA node or shared memory size (128..268435456)");
        return 0;
    }
#ifndef ALCYBE_WITH_NUMA
    if (numaNode >= 0) {
        throwJava(env, "java/lang/UnsupportedOperationException", "NUMA was not enabled at build time; install libnuma-dev and rebuild");
        return 0;
    }
#else
    if (numaNode >= 0 && (numa_available() < 0 || numaNode > numa_max_node())) {
        throwJava(env, "java/lang/IllegalArgumentException", "NUMA node is unavailable");
        return 0;
    }
#endif
    auto* ctx = new (std::nothrow) RealtimeContext();
    if (!ctx) { throwJava(env, "java/lang/OutOfMemoryError", "RealtimeContext allocation failed"); return 0; }
    const size_t bytes = (size_t(sharedMemSize) + 63) & ~size_t(63);
    void* block = nullptr;
#ifdef ALCYBE_WITH_NUMA
    if (numaNode >= 0) {
        block = numa_alloc_onnode(bytes, numaNode);
        ctx->useNumaBlock = (block != nullptr);
    }
#endif
    if (numaNode >= 0 && !block) {
        delete ctx;
        throwJava(env, "java/lang/OutOfMemoryError", "NUMA allocation failed");
        return 0;
    }
    if (!block && posix_memalign(&block, CACHE_LINE_SIZE, bytes) != 0) {
        delete ctx;
        throwJava(env, "java/lang/OutOfMemoryError", "Shared block allocation failed");
        return 0;
    }
    memset(block, 0, bytes);
    ctx->sharedBlock = static_cast<SharedMemoryBlock*>(block);
    ctx->sharedBlockSize = bytes;
    ctx->priority = priority;
    ctx->cpuAffinity = cpuAffinity;
    ctx->numaNode = numaNode;
    ctx->gcEnabled = (gcEnabled == JNI_TRUE);
    ctx->javaInstance = env->NewGlobalRef(obj);
    jclass cls = env->GetObjectClass(obj);
    ctx->onCycleMethodId = cls ? env->GetMethodID(cls, "onRealtimeCycle", "(J)V") : nullptr;
    ctx->requestGcMethodId = (cls && !env->ExceptionCheck()) ? env->GetMethodID(cls, "onIdleGcWindow", "(J)V") : nullptr;
    if (cls) env->DeleteLocalRef(cls);
    if (!ctx->javaInstance || !ctx->onCycleMethodId || !ctx->requestGcMethodId || env->ExceptionCheck()) {
        if (ctx->javaInstance) env->DeleteGlobalRef(ctx->javaInstance);
#ifdef ALCYBE_WITH_NUMA
        if (ctx->useNumaBlock) numa_free(block, bytes);
        else
#endif
        free(block);
        delete ctx;
        return 0;
    }
    int rc = pthread_mutex_init(&ctx->stateMutex, nullptr);
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    int cc = rc == 0 ? pthread_cond_init(&ctx->stateCond, &attr) : rc;
    pthread_condattr_destroy(&attr);
    if (rc || cc) {
        if (!rc) pthread_mutex_destroy(&ctx->stateMutex);
        env->DeleteGlobalRef(ctx->javaInstance);
#ifdef ALCYBE_WITH_NUMA
        if (ctx->useNumaBlock) numa_free(block, bytes);
        else
#endif
        free(block);
        delete ctx;
        throwErrno(env, "pthread mutex/condition initialization", rc ? rc : cc);
        return 0;
    }
    return static_cast<jlong>(reinterpret_cast<intptr_t>(ctx));
}
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_getSharedMemoryAddress(JNIEnv*, jobject, jlong ptr) {
    auto* ctx = context(ptr);
    return ctx ? static_cast<jlong>(reinterpret_cast<intptr_t>(ctx->sharedBlock)) : 0;
}
JNIEXPORT jobject JNICALL Java_alcybe_realtime_RealtimeThread_getSharedDirectBuffer(JNIEnv* env, jobject, jlong ptr) {
    auto* ctx = context(ptr);
    return ctx ? env->NewDirectByteBuffer(ctx->sharedBlock, static_cast<jlong>(ctx->sharedBlockSize)) : nullptr;
}
static void launch(JNIEnv* env, RealtimeContext* ctx, int mode, int64_t begin, int64_t offset, int64_t period) {
    if (!ctx) { throwJava(env, "java/lang/IllegalStateException", "Thread context is closed"); return; }
    pthread_mutex_lock(&ctx->stateMutex);
    if (ctx->joinable) {
        pthread_mutex_unlock(&ctx->stateMutex);
        throwJava(env, "java/lang/IllegalStateException", "Thread already launched; stop and join before reuse");
        return;
    }
    ctx->mode = mode;
    ctx->beginTimeNs = begin;
    ctx->offsetNs = offset;
    ctx->cycleTimeNs = period;
    put32(&ctx->state, STATE_RUNNING);
    put32(&ctx->sharedBlock->threadState, STATE_RUNNING);
    int rc = pthread_create(&ctx->threadId, nullptr, workerMain, ctx);
    if (rc) {
        put32(&ctx->state, STATE_STOPPED);
        put32(&ctx->sharedBlock->threadState, STATE_STOPPED);
        pthread_mutex_unlock(&ctx->stateMutex);
        throwErrno(env, "pthread_create", rc);
        return;
    }
    ctx->joinable = true;
    pthread_mutex_unlock(&ctx->stateMutex);
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_startRealtime(JNIEnv* env, jobject, jlong ptr) {
    launch(env, context(ptr), 1, 0, 0, 0);
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_scheduleRealtime(JNIEnv* env, jobject, jlong ptr, jlong begin, jlong offset, jlong period) {
    if (period <= 0 || offset < 0 || begin < 0 || begin > INT64_MAX - offset) {
        throwJava(env, "java/lang/IllegalArgumentException", "schedule requires positive cycle, nonnegative offset, valid start");
        return;
    }
    launch(env, context(ptr), 2, begin == 0 ? nowNanos() : begin, offset, period);
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_pauseThread(JNIEnv*, jobject, jlong ptr) {
    auto* ctx = context(ptr);
    if (!ctx) return;
    pthread_mutex_lock(&ctx->stateMutex);
    if (ctx->mode == 2 && get32(&ctx->state) == STATE_RUNNING) put32(&ctx->state, STATE_PAUSED);
    pthread_cond_broadcast(&ctx->stateCond);
    pthread_mutex_unlock(&ctx->stateMutex);
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_resumeThread(JNIEnv*, jobject, jlong ptr) {
    auto* ctx = context(ptr);
    if (!ctx) return;
    pthread_mutex_lock(&ctx->stateMutex);
    if (get32(&ctx->state) == STATE_PAUSED) put32(&ctx->state, STATE_RUNNING);
    pthread_cond_broadcast(&ctx->stateCond);
    pthread_mutex_unlock(&ctx->stateMutex);
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_stopThread(JNIEnv*, jobject, jlong ptr) {
    auto* ctx = context(ptr);
    if (!ctx) return;
    pthread_mutex_lock(&ctx->stateMutex);
    put32(&ctx->state, STATE_STOPPED);
    pthread_cond_broadcast(&ctx->stateCond);
    pthread_mutex_unlock(&ctx->stateMutex);
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_joinThread(JNIEnv* env, jobject, jlong ptr) {
    auto* ctx = context(ptr);
    if (!ctx || !ctx->joinable) return;
    if (pthread_equal(pthread_self(), ctx->threadId)) {
        throwJava(env, "java/lang/IllegalStateException", "Cannot join the real-time worker from its own callback");
        return;
    }
    int rc = pthread_join(ctx->threadId, nullptr);
    if (rc) { throwErrno(env, "pthread_join", rc); return; }
    ctx->joinable = false;
    ctx->threadId = pthread_t{};
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_destroyContext(JNIEnv* env, jobject obj, jlong ptr) {
    auto* ctx = context(ptr);
    if (!ctx) return;
    Java_alcybe_realtime_RealtimeThread_stopThread(env, obj, ptr);
    Java_alcybe_realtime_RealtimeThread_joinThread(env, obj, ptr);
    if (env->ExceptionCheck()) return;
    env->DeleteGlobalRef(ctx->javaInstance);
    pthread_cond_destroy(&ctx->stateCond);
    pthread_mutex_destroy(&ctx->stateMutex);
#ifdef ALCYBE_WITH_NUMA
    if (ctx->useNumaBlock) numa_free(ctx->sharedBlock, ctx->sharedBlockSize);
    else
#endif
    free(ctx->sharedBlock);
    delete ctx;
}
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_currentTimeNanos(JNIEnv*, jclass) { return nowNanos(); }
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_currentTimeMicros(JNIEnv*, jclass) { return nowNanos() / 1000; }
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_currentTimeMillis(JNIEnv*, jclass) { return nowNanos() / 1000000; }
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_sleepNanos(JNIEnv*, jclass, jlong nanos) {
    if (nanos <= 0) return;
    timespec ts = nanosToTime(nowNanos() + nanos);
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
}
struct alignas(CACHE_LINE_SIZE) NativeMutex { pthread_mutex_t mutex; };
struct alignas(CACHE_LINE_SIZE) NativeBarrier { pthread_barrier_t barrier; };
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_initMutex(JNIEnv* env, jclass) {
    void* memory = nullptr;
    if (posix_memalign(&memory, CACHE_LINE_SIZE, sizeof(NativeMutex)) != 0) {
        throwJava(env, "java/lang/OutOfMemoryError", "Mutex allocation failed"); return 0;
    }
    auto* nm = static_cast<NativeMutex*>(memory);
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    int prc = pthread_mutexattr_setprotocol(&attr, PTHREAD_PRIO_INHERIT);
    if (prc) { pthread_mutexattr_destroy(&attr); free(nm); throwErrno(env, "PI mutex protocol", prc); return 0; }
    int rc = pthread_mutex_init(&nm->mutex, &attr);
    pthread_mutexattr_destroy(&attr);
    if (rc) { free(nm); throwErrno(env, "pthread_mutex_init", rc); return 0; }
    return static_cast<jlong>(reinterpret_cast<intptr_t>(nm));
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_lockMutex(JNIEnv* env, jclass, jlong ptr) {
    if (!ptr) { throwJava(env, "java/lang/IllegalStateException", "Mutex is closed"); return; }
    int rc = pthread_mutex_lock(&reinterpret_cast<NativeMutex*>(static_cast<intptr_t>(ptr))->mutex);
    if (rc) throwErrno(env, "pthread_mutex_lock", rc);
}
JNIEXPORT jboolean JNICALL Java_alcybe_realtime_RealtimeThread_tryLockMutex(JNIEnv* env, jclass, jlong ptr) {
    if (!ptr) { throwJava(env, "java/lang/IllegalStateException", "Mutex is closed"); return JNI_FALSE; }
    int rc = pthread_mutex_trylock(&reinterpret_cast<NativeMutex*>(static_cast<intptr_t>(ptr))->mutex);
    if (rc != 0 && rc != EBUSY) throwErrno(env, "pthread_mutex_trylock", rc);
    return rc == 0 ? JNI_TRUE : JNI_FALSE;
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_unlockMutex(JNIEnv* env, jclass, jlong ptr) {
    if (!ptr) { throwJava(env, "java/lang/IllegalStateException", "Mutex is closed"); return; }
    int rc = pthread_mutex_unlock(&reinterpret_cast<NativeMutex*>(static_cast<intptr_t>(ptr))->mutex);
    if (rc) throwErrno(env, "pthread_mutex_unlock", rc);
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_destroyMutex(JNIEnv* env, jclass, jlong ptr) {
    if (!ptr) return;
    auto* nm = reinterpret_cast<NativeMutex*>(static_cast<intptr_t>(ptr));
    int rc = pthread_mutex_destroy(&nm->mutex);
    if (rc) { throwErrno(env, "pthread_mutex_destroy (still in use?)", rc); return; }
    free(nm);
}
JNIEXPORT jlong JNICALL Java_alcybe_realtime_RealtimeThread_initBarrier(JNIEnv* env, jclass, jint parties) {
    if (parties <= 0) { throwJava(env, "java/lang/IllegalArgumentException", "Barrier parties must be positive"); return 0; }
    void* memory = nullptr;
    if (posix_memalign(&memory, CACHE_LINE_SIZE, sizeof(NativeBarrier)) != 0) {
        throwJava(env, "java/lang/OutOfMemoryError", "Barrier allocation failed"); return 0;
    }
    auto* nb = static_cast<NativeBarrier*>(memory);
    int rc = pthread_barrier_init(&nb->barrier, nullptr, static_cast<unsigned>(parties));
    if (rc) { free(nb); throwErrno(env, "pthread_barrier_init", rc); return 0; }
    return static_cast<jlong>(reinterpret_cast<intptr_t>(nb));
}
JNIEXPORT jint JNICALL Java_alcybe_realtime_RealtimeThread_awaitBarrier(JNIEnv* env, jclass, jlong ptr) {
    if (!ptr) { throwJava(env, "java/lang/IllegalStateException", "Barrier is closed"); return -1; }
    int rc = pthread_barrier_wait(&reinterpret_cast<NativeBarrier*>(static_cast<intptr_t>(ptr))->barrier);
    if (rc != 0 && rc != PTHREAD_BARRIER_SERIAL_THREAD) { throwErrno(env, "pthread_barrier_wait", rc); return -1; }
    return rc == PTHREAD_BARRIER_SERIAL_THREAD ? 1 : 0;
}
JNIEXPORT void JNICALL Java_alcybe_realtime_RealtimeThread_destroyBarrier(JNIEnv* env, jclass, jlong ptr) {
    if (!ptr) return;
    auto* nb = reinterpret_cast<NativeBarrier*>(static_cast<intptr_t>(ptr));
    int rc = pthread_barrier_destroy(&nb->barrier);
    if (rc) { throwErrno(env, "pthread_barrier_destroy (still in use?)", rc); return; }
    free(nb);
}
} // extern C
