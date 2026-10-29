# Platform architecture

```text
Java API: RealtimeCamera, RealtimeThread
              |
             JNI
              |
     Shared native memory ABI
              |
     +--------+---------+
     |        |         |
    Linux    macOS    Windows
    V4L2     AVF       MF
    POSIX    QoS       MMCSS
    NUMA     UMA       Win32 NUMA
```

The Linux implementation stays in its original native files. The portable core is in `native/portable`. The portable camera sources implement `CameraSource`.

Each source writes 32-bit ARGB pixels into a 64-byte-aligned native frame block. The Java API reads the same byte offsets on each platform. Java copies the pixels into a reusable `BufferedImage`.

Linux uses V4L2 MMAP and preserves driver timestamps when available. macOS uses a serial AVFoundation delegate queue. Windows uses the Media Foundation synchronous Source Reader. These APIs differ in buffer ownership and timing.

The portable runtime uses `std::thread`, `std::condition_variable`, and `std::chrono::steady_clock`. macOS requests interactive QoS. Windows requests MMCSS capture priority. Neither request creates a hard scheduling deadline.

The existing GC coordinator accepts requests when the estimated idle interval is above 1 ms. It coalesces requests and enforces 50 ms between accepted requests. A separate Java daemon issues `System.gc()`. The capture worker does not wait for the call to finish.

On macOS, CPU pinning and NUMA binding are rejected. On Windows, CPU affinity applies within the current processor group. Requested NUMA blocks use `VirtualAllocExNuma`. Neither mechanism is a latency guarantee.

The native `realtime` library uses a single JNI entry point per Java method. A platform build selects only one camera source and one thread implementation.
