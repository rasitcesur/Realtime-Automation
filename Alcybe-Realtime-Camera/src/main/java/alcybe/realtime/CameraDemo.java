package alcybe.realtime;

import java.awt.image.BufferedImage;

/** Captures frames from a native camera. */
public final class CameraDemo {
    private CameraDemo() {}

    public static void main(String[] args) throws Exception {
        if (args.length < 1) {
            System.err.println("Usage: CameraDemo <deviceId> [width=640] [height=480] [fps=30] [frames=100] [ARGB|MJPEG]");
            System.exit(2);
        }
        String deviceId = args[0];
        int width = args.length > 1 ? Integer.parseInt(args[1]) : 640;
        int height = args.length > 2 ? Integer.parseInt(args[2]) : 480;
        int fps = args.length > 3 ? Integer.parseInt(args[3]) : 30;
        int frames = args.length > 4 ? Integer.parseInt(args[4]) : 100;
        int mode = args.length > 5 && "MJPEG".equalsIgnoreCase(args[5])
                ? RealtimeCamera.PIXEL_MODE_RAW_MJPEG : RealtimeCamera.PIXEL_MODE_YUYV_TO_INT_RGB;
        try (RealtimeCamera camera = new RealtimeCamera()) {
            camera.open(deviceId, width, height, fps, mode, -1);
            System.out.printf("Opened %s %dx%d @ requested %d fps %s%n", deviceId, width, height, fps,
                    mode == 0 ? "ARGB" : "MJPEG");
            int captured = 0;
            long start = System.nanoTime();
            for (int i = 0; i < frames; i++) {
                BufferedImage image = camera.capture(1000, true);
                if (image != null) {
                    captured++;
                    if (captured % 20 == 0) {
                        System.out.printf("frame=%d deviceTs=%d capture=%d us dropped=%d gcRequests=%d sample=%08x%n",
                                camera.getFrameSequence(), camera.getHardwareTimestampNanos(),
                                camera.getDutyCycleNanos() / 1000, camera.getDroppedFramesCount(),
                                camera.getGcTriggeredCount(), image.getRGB(0, 0));
                    }
                }
            }
            double seconds = (System.nanoTime() - start) / 1e9;
            System.out.printf("Captured %d/%d frames in %.3f s (%.2f fps measured)%n",
                    captured, frames, seconds, captured / seconds);
        }
    }
}
