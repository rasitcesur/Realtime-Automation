# Validation

## Completed on Linux

- GCC 14 and JDK 21 source build.
- Linux V4L2 JNI tests without a camera.
- Native YUYV-to-ARGB pixel tests.
- Portable JNI core compilation with a mock camera.
- Frame capture and scheduled callback checks using a mock source.
- Idle-GC enabled and disabled checks.

## Not yet verified

- Native macOS compilation and camera permission flow.
- Native Windows compilation and camera device negotiation.
- Real sensor timestamps and frame orientation on each platform.
- Hardware capture latency, drops, CPU affinity, NUMA locality, and GC pause tails.
- Real-time behavior under sustained CPU, I/O, and memory load.

## Suggested hardware checks

Measure frame-to-callback latency and scheduling jitter at p50, p99, and p99.9. Repeat during high CPU load. Verify all requested camera resolutions and frame rates. Inspect ZGC safepoint logs separately from camera latency.

Use system tracing when an unexpected delay appears. Do not infer a hard real-time guarantee from a smoke test.
