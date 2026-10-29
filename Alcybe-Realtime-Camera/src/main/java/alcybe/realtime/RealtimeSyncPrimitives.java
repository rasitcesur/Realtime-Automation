package alcybe.realtime;

/** Linux POSIX priority-inheritance mutex and cyclic-barrier JNI wrappers.
 * Close only when no other thread may enter or be waiting on the primitive. */
public final class RealtimeSyncPrimitives {
    private RealtimeSyncPrimitives() {}

    public static final class NativeMutexLock implements AutoCloseable {
        private volatile long mutexPtr;
        public NativeMutexLock() { mutexPtr = RealtimeThread.initMutex(); if (mutexPtr == 0) throw new OutOfMemoryError("PI mutex initialization failed"); }
        private long pointer() { long p = mutexPtr; if (p == 0) throw new IllegalStateException("Mutex closed"); return p; }
        public void lock() { RealtimeThread.lockMutex(pointer()); }
        public boolean tryLock() { return RealtimeThread.tryLockMutex(pointer()); }
        public void unlock() { RealtimeThread.unlockMutex(pointer()); }
        /** Unsafe after close; do not retain native pointers. */
        public long getPointer() { return pointer(); }
        @Override public synchronized void close() { if (mutexPtr != 0) { RealtimeThread.destroyMutex(mutexPtr); mutexPtr = 0; } }
    }
    public static final class NativeCyclicBarrier implements AutoCloseable {
        private volatile long barrierPtr;
        private final int parties;
        public NativeCyclicBarrier(int parties) {
            if (parties <= 0) throw new IllegalArgumentException("parties must be positive");
            this.parties = parties;
            barrierPtr = RealtimeThread.initBarrier(parties);
            if (barrierPtr == 0) throw new OutOfMemoryError("pthread_barrier initialization failed");
        }
        public int await() {
            long p = barrierPtr;
            if (p == 0) throw new IllegalStateException("Barrier closed");
            return RealtimeThread.awaitBarrier(p);
        }
        public int getParties() { return parties; }
        public long getPointer() { if (barrierPtr == 0) throw new IllegalStateException("Barrier closed"); return barrierPtr; }
        @Override public synchronized void close() { if (barrierPtr != 0) { RealtimeThread.destroyBarrier(barrierPtr); barrierPtr = 0; } }
    }
}
