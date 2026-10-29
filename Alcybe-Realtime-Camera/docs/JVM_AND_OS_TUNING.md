# JVM and OS tuning

Use Java 21 or later. Start with a fixed heap that fits in physical RAM.

```bash
java -XX:+UseZGC -XX:+ExplicitGCInvokesConcurrent \
  -Xms512m -Xmx2G -Djava.library.path=build/native \
  -cp build/classes alcybe.realtime.CameraDemo /dev/video0
```

For Java 21, Generational ZGC is available with `-XX:+ZGenerational`. Do not use that flag on Java 24 or later.

The scheduler estimates the idle interval after a frame. It requests GC when that interval exceeds 1 ms. The coordinator accepts at most one request every 50 ms. The GC call runs on a separate Java daemon.

ZGC reduces application pauses. It does not provide a strict 1 ms pause guarantee. Camera latency includes native capture, Java callbacks, scheduling, and buffer copies.

Linux `SCHED_FIFO` needs suitable privileges. macOS QoS and Windows MMCSS are not equivalent to `SCHED_FIFO`. Measure scheduling jitter under realistic load.
