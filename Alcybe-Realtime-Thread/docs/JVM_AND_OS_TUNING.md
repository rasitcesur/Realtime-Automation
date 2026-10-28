# Linux / HotSpot tuning notes

Generational ZGC's principal design goal is sub-millisecond Stop-The-World (STW) pauses, even though an entire concurrent collection can last much longer. This is a latency goal, not a hard upper bound for arbitrary HotSpot/Linux workloads; root causes of larger observed pauses should be logged and investigated rather than assumed impossible.

## JVM configuration

Use `./scripts/run-linux.sh` to choose runtime flags by JDK version. ZGC generational mode is selectable in JDK 21/22, becomes the default for ZGC in JDK 23, and the non-generational option is removed later; do not pass `-XX:+ZGenerational` unconditionally on new JDKs. Use `-Xms`/`-Xmx` according to actual RAM. `-XX:+AlwaysPreTouch` can increase startup time and physical memory pressure. NUMA-aware Java heap placement and native memory NUMA placement are distinct concerns.

Set `USE_LARGE_PAGES=1` only after configuring the host's huge pages. If there are no suitable huge pages, HotSpot may warn and fall back. No system configuration is applied automatically by this project.

## Original idle-triggered concurrent GC policy

The C++ scheduler calculates remaining time to its next absolute deadline. A remaining idle window **strictly above 1 ms** permits a GC request. Requests are throttled to at most one accepted request in **50 ms**, globally across workers. This request policy is on by default (`-Dalcybe.realtime.explicitGcOnIdle=false` turns it off). A dedicated Java daemon handles `System.gc()` so its potentially long calling-thread latency is never paid by the JNI real-time worker. The daemon may execute concurrently with later worker periods; an idle-window trigger is not a CPU scheduling reservation. Every worker's `gcTriggeredCount` counts accepted requests only, not completed collections.

Use `-Xlog:gc*,safepoint:file=gc.log:time,uptime,level,tags` to distinguish total collection duration from individual `Pause` events and safepoints. Benchmark tail jitter with idle GC requests enabled and disabled; consider CPU affinity for GC threads and heap headroom under production allocation rates. A low-pause collector alone cannot guarantee low latency if a real-time callback blocks on a Java allocation, a safepoint, or external synchronization.

## Scheduling permissions and CPU affinity

Inspect permitted CPUs (`taskset -pc $$`, `/proc/self/status`) and priority limits (`ulimit -r`). Linux `SCHED_FIFO` usually requires adequate `RLIMIT_RTPRIO` or `CAP_SYS_NICE`. The runtime logs a warning and falls back to the thread's inherited policy if scheduling or affinity requests fail. Do not assume real-time priority by reading the Java constructor arguments.

Using isolated CPUs, tickless kernel configurations, IRQ pinning, pre-faulting, page locking, stable CPU frequency and NUMA locality may improve tails on dedicated hosts. They require system-specific validation and can damage normal system responsiveness if applied blindly. Avoid universal copy-paste boot parameters, especially `idle=poll` or disabling all C-states.

## Suggested measurements

- Record Linux kernel, CPU topology, JVM version and runtime flags, GC logs, CPU frequency governor and background system load.
- Measure throughput, p50/p99/p99.9/p99.99 callback-start lateness, missed periods, GC pauses, and the tail of each callback's execution time.
- Compare `SCHED_OTHER` against permitted `SCHED_FIFO` on an isolated core; vary memory allocation rate, NUMA node, heap settings and CPU pinning **one change at a time**.
- Repeat runs after JIT warmup and report distributions rather than only the last `actualJitterNs` value.
- Use `cyclictest`, `perf`, `numactl --hardware`, JVM JFR/GC logs and processor counters as appropriate. Treat absolute latency guarantees as unsupported until measured on the target platform with a documented workload.

## Safety

Do not perform CPU isolation, RT limit, huge page or kernel boot changes on production servers without a rollback plan. Do not run unbounded loops at `SCHED_FIFO` priority 99 on cores that also service OS interrupts. Java callbacks run in JVM-attached native threads and remain subject to JVM safepoints/GC synchronization.
