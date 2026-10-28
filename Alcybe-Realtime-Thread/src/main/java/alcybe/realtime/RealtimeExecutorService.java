package alcybe.realtime;

/** Bounded preallocated thread registration and coordinated scheduling. */
public final class RealtimeExecutorService implements AutoCloseable {
    private final RealtimeThreadContainer[] containers;
    private int count;
    private boolean closed;

    public RealtimeExecutorService(int maxThreads) {
        if (maxThreads <= 0) throw new IllegalArgumentException("maxThreads must be positive");
        containers = new RealtimeThreadContainer[maxThreads];
        for (int i = 0; i < maxThreads; i++) containers[i] = new RealtimeThreadContainer();
    }

    public synchronized boolean registerScheduledTask(RealtimeThread thread, long timeOffsetNs, long cycleTimeNs) {
        if (closed) throw new IllegalStateException("Executor is closed");
        if (thread == null || timeOffsetNs < 0 || cycleTimeNs <= 0) throw new IllegalArgumentException("Invalid task/schedule");
        for (int i = 0; i < count; i++) if (containers[i].thread == thread) throw new IllegalArgumentException("Thread already registered");
        if (count == containers.length) return false;
        containers[count++].set(thread, timeOffsetNs, cycleTimeNs);
        return true;
    }
    public synchronized void startAll() { startAll(RealtimeThread.currentTimeNanos() + 5_000_000L); }
    public synchronized void startAll(long beginTimeNs) {
        if (closed) throw new IllegalStateException("Executor is closed");
        for (int i = 0; i < count; i++) containers[i].thread.schedule(beginTimeNs, containers[i].timeOffsetNs, containers[i].cycleTimeNs);
    }
    public synchronized void pauseAll() { for (int i = 0; i < count; i++) containers[i].thread.pause(); }
    public synchronized void resumeAll() { for (int i = 0; i < count; i++) containers[i].thread.resume(); }
    public synchronized void stopAll() { for (int i = 0; i < count; i++) containers[i].thread.stop(); }
    public synchronized void joinAll() { for (int i = 0; i < count; i++) containers[i].thread.join(); }
    @Override public synchronized void close() {
        if (closed) return;
        stopAll();
        joinAll();
        for (int i = 0; i < count; i++) { containers[i].thread.close(); containers[i].clear(); }
        count = 0;
        closed = true;
    }
}
