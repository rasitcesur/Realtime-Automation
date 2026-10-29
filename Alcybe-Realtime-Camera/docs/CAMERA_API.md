# Camera API

`RealtimeCamera` uses a shared Java interface on Linux, macOS, and Windows.

```java
try (RealtimeCamera camera = new RealtimeCamera()) {
    camera.open("default", 640, 480, 30,
        RealtimeCamera.PIXEL_MODE_YUYV_TO_INT_RGB, -1);
    BufferedImage image = camera.capture(1000, true);
    if (image != null) {
        int pixelArgb = image.getRGB(0, 0);
    }
}
```

On Linux, use `/dev/video0` or another V4L2 path. On macOS, use `default` or a device unique ID. On Windows, use `default`, a numeric camera index, or a symbolic link.

The legacy `PIXEL_MODE_YUYV_TO_INT_RGB` constant selects the ARGB output pipeline. Linux obtains YUYV input. macOS and Windows obtain native BGRA or RGB32 input. The Java output remains ARGB.

Linux supports `PIXEL_MODE_RAW_MJPEG` and the `V4L2_CID_*` controls. These operations are unavailable on macOS and Windows. A control query returns `false` or `-1` there. Raw MJPEG opens throw `UnsupportedOperationException`.

The returned image is reused. Copy it before keeping it beyond the next capture. The raw address and direct buffer are invalid after `close()`.

Scheduled capture callbacks run on a native-attached JVM daemon thread. Do not close the camera from its own callback. The scheduler uses a monotonic clock. Native timestamps on macOS and Windows represent frame arrival time, not a hardware sensor clock.
