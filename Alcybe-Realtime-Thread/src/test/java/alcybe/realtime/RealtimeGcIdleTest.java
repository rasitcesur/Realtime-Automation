package alcybe.realtime;

import java.util.concurrent.TimeUnit;

/** Checks both branches of the native 1-ms idle-window and explicit-GC policy. */
public final class RealtimeGcIdleTest {
    private static void waitForCycles(RealtimeThread worker, long n) throws Exception {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(6);
        while (worker.getSequenceNumber() < n && System.nanoTime() < deadline) {
            Thread.sleep(2);
        }
        if (worker.getSequenceNumber() < n) {
            throw new AssertionError("Insufficient cycles " + worker.getSequenceNumber());
        }
    }
    public static void main(String[] args) throws Exception {
        boolean enabled = Boolean.parseBoolean(System.getProperty("alcybe.realtime.explicitGcOnIdle", "true"));
        // A 0.5-ms period can never provide >1 ms of idle time.
        try (RealtimeThread shortCycle = new RealtimeThread((a, b) -> {}, 0, -1)) {
            shortCycle.schedule(0, 0, 500_000L);
            waitForCycles(shortCycle, 100);
            shortCycle.stop();
            shortCycle.join();
            if (shortCycle.getGcTriggeredCount() != 0) throw new AssertionError("GC requested without idle window");
        }
        // A 5-ms period provides >1 ms of idle time to request a collection.
        try (RealtimeThread longCycle = new RealtimeThread((a, b) -> {}, 0, -1)) {
            longCycle.schedule(0, 0, 5_000_000L);
            waitForCycles(longCycle, 35);
            longCycle.stop();
            longCycle.join();
            int accepted = longCycle.getGcTriggeredCount();
            if (enabled && accepted == 0) throw new AssertionError("Eligible GC requests were not accepted");
            if (!enabled && accepted != 0) throw new AssertionError("Disabled GC was requested");
            System.out.println("PASS: idle GC enabled=" + enabled + " accepted requests=" + accepted);
        }
    }
}
