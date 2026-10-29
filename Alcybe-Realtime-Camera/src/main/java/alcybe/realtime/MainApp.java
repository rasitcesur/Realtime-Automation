package alcybe.realtime;

/** Executable smoke/demo: no RT privileges needed, uses SCHED_OTHER by default. */
public final class MainApp {
    public static void main(String[] args) throws InterruptedException {
        final int cycles = 100;
        try (RealtimeThread worker = new RealtimeThread((address, buffer) -> {}, 0, -1)) {
            long begin = RealtimeThread.currentTimeNanos() + 10_000_000L;
            worker.schedule(begin, 0, 2_000_000L);
            long deadline = System.nanoTime() + 5_000_000_000L;
            while (worker.getSequenceNumber() < cycles && System.nanoTime() < deadline) Thread.sleep(2);
            worker.stop();
            worker.join();
            System.out.printf("Cycles=%d last-duty-ns=%d last-jitter-ns=%d state=%d%n",
                    worker.getSequenceNumber(), worker.getDutyCycleNanos(), worker.getActualJitterNanos(), worker.getThreadState());
            if (worker.getSequenceNumber() < cycles) throw new IllegalStateException("Demo cycle count not reached");
        }
    }
}
