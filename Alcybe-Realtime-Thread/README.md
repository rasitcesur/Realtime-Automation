# Alcybe Real-Time

Linux x86-64 JNI/POSIX soft real-time scheduler for Java applications. The project provides periodic and one-shot native threads, deadline-based scheduling with `CLOCK_MONOTONIC`, optional `SCHED_FIFO` and CPU affinity, optional NUMA allocation, a 64-byte aligned shared-memory header, `ByteBuffer`/`VarHandle` accessors, priority-inheritance mutexes, and cyclic barriers.

> **Status:** Working JNI proof of concept / experimental systems software. It is **not a hard real-time JVM**, a safety-critical runtime, or a promise of bounded GC pauses or microsecond jitter. See [limitations](#operational-limitations).

## Requirements

- Linux x86-64, GCC/G++ supporting C++17, Bash, and OpenJDK **21 or newer**.
- Optional `libnuma-dev` for NUMA-aware allocation (`libnuma1` at runtime).
- Optional real-time scheduler permissions (e.g. `RLIMIT_RTPRIO` or `CAP_SYS_NICE`) and appropriate allowed CPU set. The smoke tests do **not** require root.

Ubuntu/Debian:

```bash
sudo apt-get update
sudo apt-get install -y build-essential openjdk-21-jdk libnuma-dev
```

## Build and test

```bash
./scripts/build-linux.sh
./scripts/test-linux.sh  # includes both idle-GC enabled/disabled branches
# Optional: collect empirical callback-start jitter distribution (2000 cycles, 1ms period)
./scripts/bench-linux.sh 2000 0 -1
```

On machines without `libnuma-dev`, the build deliberately falls back to no NUMA support. Requests for `numaNode >= 0` then fail with a clear exception. Use `MARCH_NATIVE=0 ./scripts/build-linux.sh` to produce a portable binary for the target OS and ABI instead of optimizing for the build host's ISA.

For a manual C++ build from the repository root, after installing `libnuma-dev`:

```bash
export JAVA_HOME="$(dirname "$(dirname "$(readlink -f "$(command -v javac)")")")"
g++ -std=c++17 -O3 -march=native -flto -shared -fPIC -pthread \
  -DALCYBE_WITH_NUMA=1 \
  -I"$JAVA_HOME/include" -I"$JAVA_HOME/include/linux" \
  native/alcybe_realtime_RealtimeThread.cpp \
  -o build/native/librealtime.so -lnuma -lrt
```

The original `-lnuma` command needs the development package, not just the `libnuma.so.1` runtime. `-lpthread` is typically equivalent to linking with `-pthread` on modern Linux toolchains; `-pthread` also sets compile-time thread options.

### Run the demo

```bash
HEAP_MIN=128m HEAP_MAX=128m ./scripts/run-linux.sh
```

With sufficient system RAM and correctly configured OS limits, the default production-sized demo uses `-Xms8G -Xmx8G`:

```bash
./scripts/run-linux.sh
```

The script uses `-XX:+UseZGC`, `-XX:+AlwaysPreTouch`, `-XX:+UseNUMA`, and `-XX:+ExplicitGCInvokesConcurrent`. It adds `-XX:+ZGenerational` on JDK 21/22 only (generational mode is the default on JDK 23+), and does **not** use the obsolete `UseBiasedLocking` option. `USE_LARGE_PAGES=1` opts into huge pages only when configured on the host. Set `JAVA_EXTRA_OPTS` for additional JVM tuning and `MAIN_CLASS` to run another Java entry point.

A direct JDK 21 invocation of the default demo is:

```bash
java -XX:+UseZGC -XX:+ZGenerational \
  -Xms8G -Xmx8G -XX:+AlwaysPreTouch \
  -XX:+UseNUMA -XX:+ExplicitGCInvokesConcurrent \
  -Djava.library.path=build/native -cp build/classes \
  alcybe.realtime.MainApp
```

Note that `-cp build/classes` is required unless your CLASSPATH already includes the compiled classes; `java alcybe.realtime.MainApp` alone cannot find classes in `build/classes`.

## Java example

```java
import alcybe.realtime.RealtimeThread;

try (RealtimeThread worker = new RealtimeThread(
        (address, buffer) -> {
            // Keep callbacks short, preallocated, and nonblocking when possible.
        }, 0, -1)) { // priority 0 = SCHED_OTHER; cpu -1 = no pinning
    worker.schedule(RealtimeThread.currentTimeNanos() + 10_000_000L,
                    0, 1_000_000L); // 1 ms period
    RealtimeThread.sleepNanos(20_000_000L);
    worker.stop();
    worker.join();
    System.out.println("Cycles: " + worker.getSequenceNumber());
}
```

`priority=1..99` requests `SCHED_FIFO`; if the kernel rejects the request the implementation logs a warning and continues with the inherited scheduler policy. It does **not** silently claim real-time scheduling was established. Choose CPU affinity only among CPUs permitted by the current process/cgroup. `numaNode=-1` avoids an explicit NUMA allocation request. The native worker is attached to the JVM, so Java heap allocation, safepoints, class loading, and JIT-related interruptions remain possible.

## Lifecycle and memory contract

- A native worker is created using `pthread_create`, and must be stopped (for a periodic worker) and joined before restarting it. `close()` stops, joins and releases its native memory. Repeated `close()` is safe.
- `pause()` is cooperative: the current callback may finish before the pause is effective. `resume()` schedules the next callback one period in the future. Overruns skip missed periods; they do not enqueue callback backlog.
- Callback exceptions are recorded in `getFailure()` and stop the scheduler rather than propagating across a JNI-attached native thread.
- `getSharedBuffer()` and `getRawMemoryAddress()` expose **native off-heap memory**. **Do not retain or access either after `close()`**; accessing a stale direct buffer/pointer can crash the entire JVM. Only close when other application threads have stopped using this memory.
- `NativeMutexLock` and `NativeCyclicBarrier` must not be destroyed while another thread holds or waits on them. The low-level pointer API is inherently unsafe if concurrently closed.
- Shared region size is total allocation size, **including** the 64-byte header, not payload-only size. The default is 4096 bytes. Payload field accesses enforce width alignment and bounds. All shared numeric values use native byte order.
- The header's sequence number and telemetry are written using native atomic operations and read via Java `VarHandle` volatile/acquire operations. The cross-language memory layout is defined explicitly in `native/alcybe_realtime_RealtimeThread.h`.

## Operational limitations

- **Best-effort/soft real-time only:** neither Java, Linux userspace, ZGC, `SCHED_FIFO`, CPU isolation nor NUMA binding alone establishes hard upper bounds on execution time, scheduling delay or end-to-end jitter.
- **Original idle-GC policy is retained (enabled by default):** when a native worker observes **more than 1 ms of remaining idle time**, it offers a GC request at most once every **50 ms** (globally). A single non-real-time daemon coalesces requests and calls `System.gc()`, rather than blocking the deadline-critical worker. The per-thread `gcTriggeredCount` counts accepted requests, **not** finished collections. Set `-Dalcybe.realtime.explicitGcOnIdle=false` to disable and benchmark the JVM's automatic GC behavior instead.
- **GC cycle length and STW pause length differ.** Generational ZGC normally keeps STW pauses sub-millisecond while concurrent collection may take longer. `System.gc()` can block its calling thread, which is why this repository's call runs on a coordinator thread. Neither the 1-ms idle *threshold* nor Generational ZGC constitute a hard upper bound on OS/JVM scheduling pauses. The coordinator can still consume CPU during subsequent duty cycles; test with and without explicit requests under production allocation loads.
- One-shot and periodic callbacks execute on JVM-attached native threads. `RealtimeTask.execute` must itself avoid blocking, unpredictable I/O and allocations if low jitter matters.
- `-XX:+UseLargePages` requires kernel support and appropriate huge-page configuration; the script does not enable it by default.
- `mlockall(MCL_CURRENT|MCL_FUTURE)` is **not automatically performed**: when `RLIMIT_MEMLOCK` is too low it can make later JVM allocations fail. Configure this at process/host level only after measuring memory requirements and available limits.
- If unprivileged, `pthread_setschedparam(SCHED_FIFO)` can fail with `EPERM`; the runtime warns. `CAP_SYS_NICE`/`RLIMIT_RTPRIO` policy is OS-specific. Running the full JVM as root is discouraged.
- Periodic schedule telemetry records last callback *start lateness*, duty time, and time to next planned deadline. It does not capture worst-case tail latency or separate callback execution from JNI/VM overhead.

See [docs/JVM_AND_OS_TUNING.md](docs/JVM_AND_OS_TUNING.md) for kernel/JVM tuning guidance and benchmarking limits.

## Project structure

```text
native/                     C++17 JNI implementation and ABI header
src/main/java/alcybe/realtime/
                            Java API, initializer, sample MainApp
src/test/java/alcybe/realtime/
                            Self-contained JNI integration smoke test
scripts/build-linux.sh      Native and Java build
scripts/test-linux.sh       Build plus integration tests
scripts/bench-linux.sh      Empirical p50/p99/p99.9 callback-start lateness
scripts/run-linux.sh        Version-aware ZGC launch helper
docs/                       JVM/Linux tuning and test notes
.github/workflows/          CI on OpenJDK 21 and 25
```

## License

MIT, see [`LICENSE`](LICENSE). Contributions are welcome subject to the same license. This distribution includes source code only; local build outputs are ignored by Git.
