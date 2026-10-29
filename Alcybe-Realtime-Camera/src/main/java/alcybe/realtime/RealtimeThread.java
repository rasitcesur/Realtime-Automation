package alcybe.realtime;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.Objects;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.locks.LockSupport;

/** Runs native workers on supported desktop platforms. */
public final class RealtimeThread implements AutoCloseable {
    static {
        RealtimeInitializer.init();
        initSystem(0);
    }

    public static final int OFFSET_SEQUENCE_NUMBER = 0;
    public static final int OFFSET_DUTY_CYCLE_NS = 8;
    public static final int OFFSET_IDLE_CYCLE_NS = 16;
    public static final int OFFSET_ACTUAL_JITTER_NS = 24;
    public static final int OFFSET_THREAD_STATE = 32;
    public static final int OFFSET_GC_COUNT = 36;
    public static final int OFFSET_PAYLOAD = 64;

    public static final int STATE_STOPPED = 0;
    public static final int STATE_RUNNING = 1;
    public static final int STATE_PAUSED = 2;

    private static final VarHandle LONG_HANDLE = MethodHandles.byteBufferViewVarHandle(long[].class, ByteOrder.nativeOrder());
    private static final VarHandle INT_HANDLE = MethodHandles.byteBufferViewVarHandle(int[].class, ByteOrder.nativeOrder());
    // Enable idle-window GC requests by default.
    private static final boolean EXPLICIT_GC_ON_IDLE = Boolean.parseBoolean(
            System.getProperty("alcybe.realtime.explicitGcOnIdle", "true"));
    private static final long MIN_GC_INTERVAL_NS = 50_000_000L;
    private static final long GC_IDLE_THRESHOLD_NS = 1_000_000L;

    private volatile long contextPtr;
    private volatile long sharedMemoryAddress;
    private volatile ByteBuffer directBuffer;
    private final RealtimeTask task;
    private final int bufferBytes;
    private volatile Throwable failure;
    private volatile boolean closing;

    @FunctionalInterface
    public interface RealtimeTask {
        void execute(long rawMemoryAddress, ByteBuffer sharedBuffer);
    }

    public RealtimeThread(RealtimeTask task, int priority, int cpuAffinity, int numaNode, int sharedMemorySize) {
        this.task = Objects.requireNonNull(task, "task");
        // Start the GC daemon before the worker.
        if (EXPLICIT_GC_ON_IDLE) IdleGcCoordinator.ensureStarted();
        this.bufferBytes = sharedMemorySize;
        this.contextPtr = createContext(priority, cpuAffinity, numaNode, sharedMemorySize, EXPLICIT_GC_ON_IDLE);
        if (contextPtr == 0) throw new OutOfMemoryError("Failed to initialize native RealtimeContext");
        try {
            sharedMemoryAddress = getSharedMemoryAddress(contextPtr);
            ByteBuffer buffer = getSharedDirectBuffer(contextPtr);
            if (buffer == null) throw new OutOfMemoryError("NewDirectByteBuffer failed");
            directBuffer = buffer.order(ByteOrder.nativeOrder());
        } catch (Throwable t) {
            destroyContext(contextPtr);
            contextPtr = 0;
            throw t;
        }
    }

    public RealtimeThread(RealtimeTask task, int priority, int cpuAffinity) {
        this(task, priority, cpuAffinity, -1, 4096);
    }

    /** Runs the callback and records failures. */
    @SuppressWarnings("unused")
    private void onRealtimeCycle(long rawAddress) {
        try {
            task.execute(rawAddress, directBuffer);
        } catch (Throwable t) {
            failure = t;
            stopThread(contextPtr);
        }
    }

    /** Queues GC when the idle window is sufficient. */
    @SuppressWarnings("unused")
    private void onIdleGcWindow(long availableIdleNs) {
        if (EXPLICIT_GC_ON_IDLE && availableIdleNs > GC_IDLE_THRESHOLD_NS
                && IdleGcCoordinator.offer()) {
            ByteBuffer buffer = directBuffer;
            if (buffer != null) INT_HANDLE.getAndAdd(buffer, OFFSET_GC_COUNT, 1);
        }
    }

    /** Coordinates GC requests on a dedicated daemon. */
    private static final class IdleGcCoordinator {
        private static final AtomicBoolean BUSY = new AtomicBoolean(false);
        private static volatile long lastRequestNs;
        static {
            Thread worker = new Thread(() -> {
                while (true) {
                    if (!BUSY.get()) {
                        // Poll without waking the native worker.
                        LockSupport.parkNanos(1_000_000L);
                        continue;
                    }
                    try {
                        System.gc();
                    } catch (Throwable t) {
                        // A failed explicit request must not terminate the coordinator.
                        System.err.println("[alcybe-realtime] idle GC request failed: " + t);
                    } finally {
                        BUSY.set(false);
                    }
                }
            }, "Alcybe-Idle-ZGC");
            worker.setDaemon(true);
            worker.start();
        }
        static void ensureStarted() { /* triggers class initialization on a non-RT thread */ }
        static boolean offer() {
            long now = System.nanoTime();
            if (now - lastRequestNs < MIN_GC_INTERVAL_NS) return false;
            if (!BUSY.compareAndSet(false, true)) return false;
            lastRequestNs = now;
            return true;
        }
    }

    /** Initialize the shared GC daemon outside any deadline-critical native callback. */
    public static void prepareIdleGcCoordinator() {
        if (EXPLICIT_GC_ON_IDLE) IdleGcCoordinator.ensureStarted();
    }

    /** Coalesces all camera and RT worker GC requests, preserving the 1 ms idle gate. */
    public static boolean requestIdleGc(long availableIdleNs) {
        return EXPLICIT_GC_ON_IDLE && availableIdleNs > GC_IDLE_THRESHOLD_NS
                && IdleGcCoordinator.offer();
    }

    public Throwable getFailure() { return failure; }

    private ByteBuffer buffer() {
        ByteBuffer buffer = directBuffer;
        if (buffer == null || contextPtr == 0 || closing) throw new IllegalStateException("RealtimeThread is closed");
        return buffer;
    }

    private void checkPayload(int offset, int width) {
        if (offset < 0 || offset % width != 0 || (long) offset + width > (long) bufferBytes - OFFSET_PAYLOAD)
            throw new IndexOutOfBoundsException("Unaligned or out-of-range payload offset: " + offset);
    }

    public long getSequenceNumber() { return (long) LONG_HANDLE.getVolatile(buffer(), OFFSET_SEQUENCE_NUMBER); }
    public long getDutyCycleNanos() { return (long) LONG_HANDLE.getVolatile(buffer(), OFFSET_DUTY_CYCLE_NS); }
    public long getIdleCycleNanos() { return (long) LONG_HANDLE.getVolatile(buffer(), OFFSET_IDLE_CYCLE_NS); }
    public long getActualJitterNanos() { return (long) LONG_HANDLE.getVolatile(buffer(), OFFSET_ACTUAL_JITTER_NS); }
    public int getThreadState() { return (int) INT_HANDLE.getVolatile(buffer(), OFFSET_THREAD_STATE); }
    public int getGcTriggeredCount() { return (int) INT_HANDLE.getVolatile(buffer(), OFFSET_GC_COUNT); }
    public void writePayloadLong(int offset, long value) { checkPayload(offset, Long.BYTES); LONG_HANDLE.setRelease(buffer(), OFFSET_PAYLOAD + offset, value); }
    public long readPayloadLong(int offset) { checkPayload(offset, Long.BYTES); return (long) LONG_HANDLE.getAcquire(buffer(), OFFSET_PAYLOAD + offset); }
    public void writePayloadInt(int offset, int value) { checkPayload(offset, Integer.BYTES); INT_HANDLE.setRelease(buffer(), OFFSET_PAYLOAD + offset, value); }
    public int readPayloadInt(int offset) { checkPayload(offset, Integer.BYTES); return (int) INT_HANDLE.getAcquire(buffer(), OFFSET_PAYLOAD + offset); }
    public long getRawMemoryAddress() { buffer(); return sharedMemoryAddress; }
    public ByteBuffer getSharedBuffer() { return buffer().duplicate().order(ByteOrder.nativeOrder()); }

    private long openContext() {
        long ptr = contextPtr;
        if (ptr == 0 || closing) throw new IllegalStateException("RealtimeThread is closed or closing");
        return ptr;
    }
    public synchronized void start() { failure = null; startRealtime(openContext()); }
    public synchronized void schedule(long beginTimeNs, long offsetNs, long cycleTimeNs) {
        failure = null;
        scheduleRealtime(openContext(), beginTimeNs, offsetNs, cycleTimeNs);
    }
    public synchronized void pause() { pauseThread(openContext()); }
    public synchronized void resume() { resumeThread(openContext()); }
    public synchronized void stop() { if (contextPtr != 0 && !closing) stopThread(contextPtr); }
    /** Stop the worker before joining. */
    public synchronized void join() { if (contextPtr != 0 && !closing) joinThread(contextPtr); }

    @Override public void close() {
        final long ptr;
        synchronized (this) {
            if (contextPtr == 0 || closing) return;
            closing = true;
            ptr = contextPtr;
        }
        try {
            // Do not hold the Java monitor while waiting for the callback to finish.
            destroyContext(ptr);
        } catch (Throwable t) {
            closing = false; // Allow a later retry if native join detected an illegal self-join.
            throw t;
        }
        synchronized (this) {
            contextPtr = 0;
            sharedMemoryAddress = 0;
            directBuffer = null;
        }
    }

    private static native void initSystem(int priority);
    private native long createContext(int priority, int cpuAffinity, int numaNode, int sharedMemSize, boolean gcEnabled);
    private native long getSharedMemoryAddress(long ctxPtr);
    private native ByteBuffer getSharedDirectBuffer(long ctxPtr);
    private native void startRealtime(long ctxPtr);
    private native void scheduleRealtime(long ctxPtr, long beginTimeNs, long offsetNs, long cycleTimeNs);
    private native void pauseThread(long ctxPtr);
    private native void resumeThread(long ctxPtr);
    private native void stopThread(long ctxPtr);
    private native void joinThread(long ctxPtr);
    private native void destroyContext(long ctxPtr);

    public static native long currentTimeNanos();
    public static native long currentTimeMicros();
    public static native long currentTimeMillis();
    public static native void sleepNanos(long nanos);
    public static native long initMutex();
    public static native void lockMutex(long mutexPtr);
    public static native boolean tryLockMutex(long mutexPtr);
    public static native void unlockMutex(long mutexPtr);
    public static native void destroyMutex(long mutexPtr);
    public static native long initBarrier(int parties);
    public static native int awaitBarrier(long barrierPtr);
    public static native void destroyBarrier(long barrierPtr);
}
