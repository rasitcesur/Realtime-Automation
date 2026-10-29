package alcybe.realtime;

import java.util.Arrays;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Optional empirical jitter benchmark; excludes warmup from reported samples. */
public final class RealtimeBenchmark {
    public static void main(String[] args) throws InterruptedException {
        int cycles = args.length > 0 ? Integer.parseInt(args[0]) : 2_000;
        int priority = args.length > 1 ? Integer.parseInt(args[1]) : 0;
        int cpu = args.length > 2 ? Integer.parseInt(args[2]) : -1;
        if (cycles < 100 || cycles > 2_000_000) throw new IllegalArgumentException("cycles must be 100..2000000");
        final long[] lateness = new long[cycles];
        final AtomicInteger index = new AtomicInteger();
        try (RealtimeThread worker = new RealtimeThread((addr, buffer) -> {
            int i = index.get();
            if (i < lateness.length) {
                lateness[i] = buffer.getLong(RealtimeThread.OFFSET_ACTUAL_JITTER_NS);
                index.set(i + 1);
            }
        }, priority, cpu)) {
            worker.schedule(RealtimeThread.currentTimeNanos() + 20_000_000L, 0, 1_000_000L);
            long timeout = System.nanoTime() + TimeUnit.SECONDS.toNanos(Math.max(10, cycles / 250));
            while (index.get() < cycles && System.nanoTime() < timeout) Thread.sleep(2);
            worker.stop();
            worker.join();
            int total = Math.min(cycles, index.get());
            if (total < cycles) throw new AssertionError("Benchmark timed out; captured " + total);
            int warmup = Math.min(total / 10, 200);
            long[] samples = Arrays.copyOfRange(lateness, warmup, total);
            Arrays.sort(samples);
            System.out.printf("period=1ms samples=%d warmup_excluded=%d priority_requested=%d cpu_requested=%d%n",
                    samples.length, warmup, priority, cpu);
            System.out.printf("lateness ns: p50=%d p99=%d p99.9=%d max=%d%n",
                    quantile(samples, .50), quantile(samples, .99), quantile(samples, .999), samples[samples.length - 1]);
            System.out.println("Diagnostic only: not a guarantee and not an independent latency trace.");
        }
    }
    private static long quantile(long[] a, double q) {
        int index = (int) Math.ceil(q * a.length) - 1;
        return a[Math.max(0, Math.min(a.length - 1, index))];
    }
}
