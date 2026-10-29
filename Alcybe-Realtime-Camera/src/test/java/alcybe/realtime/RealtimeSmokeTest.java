package alcybe.realtime;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

/** Self-contained JNI integration tests: no JUnit/network/RT privileges required. */
public final class RealtimeSmokeTest {
    private static int tests;
    private static void check(boolean ok, String description) {
        if (!ok) throw new AssertionError(description);
        tests++;
    }
    private static void expect(Class<? extends Throwable> type, Runnable action) {
        try { action.run(); }
        catch (Throwable ex) { check(type.isInstance(ex), "wrong exception " + ex); return; }
        throw new AssertionError("Expected " + type.getName());
    }
    private static void untilCycles(RealtimeThread worker, long desired) throws InterruptedException {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(3);
        while (worker.getSequenceNumber() < desired && System.nanoTime() < deadline) Thread.sleep(1);
        check(worker.getSequenceNumber() >= desired, "insufficient cycles: " + worker.getSequenceNumber());
    }
    public static void main(String[] args) throws Exception {
        check(RealtimeThread.currentTimeNanos() > 0, "monotonic clock");
        long start = RealtimeThread.currentTimeNanos();
        RealtimeThread.sleepNanos(100_000);
        check(RealtimeThread.currentTimeNanos() >= start + 100_000, "absolute sleep");
        check(RealtimeThread.currentTimeMicros() > 0 && RealtimeThread.currentTimeMillis() > 0, "clock units");

        expect(IllegalArgumentException.class, () -> new RealtimeThread((p,b)->{}, 0, -1, -1, 64));
        expect(IllegalArgumentException.class, () -> new RealtimeThread((p,b)->{}, 100, -1));
        expect(NullPointerException.class, () -> new RealtimeThread(null, 0, -1));
        expect(IllegalArgumentException.class, () -> new RealtimeSyncPrimitives.NativeCyclicBarrier(0));

        AtomicInteger counter = new AtomicInteger();
        try (RealtimeThread worker = new RealtimeThread((address, data) -> {
            if (address == 0 || data.capacity() < 128 || data.order() != ByteOrder.nativeOrder()) throw new AssertionError("direct buffer layout");
            counter.incrementAndGet();
        }, 0, -1, -1, 256)) {
            check(worker.getRawMemoryAddress() % 64 == 0, "cache alignment");
            check(worker.getSharedBuffer().isDirect(), "JNI direct buffer");
            worker.writePayloadLong(0, Long.MAX_VALUE - 5);
            check(worker.readPayloadLong(0) == Long.MAX_VALUE - 5, "long payload");
            worker.writePayloadInt(8, 123456);
            check(worker.readPayloadInt(8) == 123456, "int payload");
            expect(IndexOutOfBoundsException.class, () -> worker.readPayloadLong(2));
            expect(IndexOutOfBoundsException.class, () -> worker.writePayloadInt(4096, 7));
            expect(IllegalArgumentException.class, () -> worker.schedule(0, 0, 0));
            worker.schedule(RealtimeThread.currentTimeNanos() + 10_000_000L, 0, 1_000_000L);
            expect(IllegalStateException.class, worker::start);
            untilCycles(worker, 12);
            worker.pause();
            Thread.sleep(20);
            long paused = worker.getSequenceNumber();
            Thread.sleep(10);
            check(worker.getSequenceNumber() == paused, "pause quiescence");
            worker.resume();
            untilCycles(worker, paused + 6);
            worker.stop();
            worker.join();
            check(worker.getThreadState() == RealtimeThread.STATE_STOPPED, "stop/join");
            check(worker.getGcTriggeredCount() == 0, "no idle GC request for <=1ms available window");
            check(counter.get() >= paused + 6, "callbacks counted");
            worker.start(); // One-shot reuse after join.
            worker.join();
            check(worker.getSequenceNumber() >= paused + 7, "one-shot reuse");
        }

        try (RealtimeThread slow = new RealtimeThread((p, b)->{}, 0, -1)) {
            slow.schedule(RealtimeThread.currentTimeNanos() + TimeUnit.SECONDS.toNanos(4), 0, TimeUnit.SECONDS.toNanos(5));
            long t0 = System.nanoTime();
            slow.stop();
            slow.join();
            check(System.nanoTime() - t0 < TimeUnit.SECONDS.toNanos(2), "responsive stop of sleeping thread");
        }
        try (RealtimeThread throwing = new RealtimeThread((p,b) -> { throw new RuntimeException("intentional"); }, 0, -1)) {
            throwing.schedule(0, 0, 1_000_000L);
            long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(2);
            while (throwing.getFailure() == null && System.nanoTime() < deadline) Thread.sleep(1);
            throwing.join();
            check(throwing.getFailure() != null, "callback failure captured");
            check(throwing.getThreadState() == 0, "callback error stops scheduler");
        }

        try (RealtimeSyncPrimitives.NativeMutexLock mutex = new RealtimeSyncPrimitives.NativeMutexLock()) {
            mutex.lock();
            check(mutex.tryLock(), "recursive mutex");
            mutex.unlock();
            mutex.unlock();
        }
        try (RealtimeSyncPrimitives.NativeCyclicBarrier barrier = new RealtimeSyncPrimitives.NativeCyclicBarrier(2)) {
            int[] serial = {0,0};
            Thread second = new Thread(() -> serial[1] = barrier.await());
            second.start();
            serial[0] = barrier.await();
            second.join(3000);
            check(!second.isAlive(), "barrier released both");
            check(serial[0] + serial[1] == 1, "barrier one serial leader");
        }
        try (RealtimeExecutorService service = new RealtimeExecutorService(2)) {
            RealtimeThread a = new RealtimeThread((p,b)->{}, 0, -1);
            RealtimeThread b = new RealtimeThread((p,buffer)->{}, 0, -1);
            check(service.registerScheduledTask(a, 0, 2_000_000L), "register a");
            check(service.registerScheduledTask(b, 0, 3_000_000L), "register b");
            expect(IllegalArgumentException.class, () -> service.registerScheduledTask(a, 0, 1000));
            service.startAll();
            untilCycles(a, 3);
            untilCycles(b, 3);
            service.pauseAll();
            service.resumeAll();
            service.stopAll();
            service.joinAll();
        }
        // Repeat lifetime management to detect gross leaks and JNI global-ref lifecycle defects.
        for (int i = 0; i < 100; i++) {
            RealtimeThread temp = new RealtimeThread((p,b)->{}, 0, -1);
            temp.start();
            temp.join();
            temp.close();
            expect(IllegalStateException.class, temp::start);
        }
        System.out.println("PASS: " + tests + " JNI smoke-test assertions");
    }
}
