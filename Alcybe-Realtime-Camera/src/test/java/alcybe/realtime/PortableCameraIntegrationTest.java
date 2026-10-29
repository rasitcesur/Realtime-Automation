package alcybe.realtime;

import java.awt.image.BufferedImage;
import java.util.concurrent.atomic.AtomicInteger;

/** Tests the portable camera core with a generated frame. */
public final class PortableCameraIntegrationTest {
    private static void check(boolean valid, String message) {
        if (!valid) throw new AssertionError(message);
    }

    public static void main(String[] arguments) throws Exception {
        AtomicInteger callbackCount = new AtomicInteger();
        try (RealtimeCamera camera = new RealtimeCamera() {
            @Override
            public void onFrameCaptured(BufferedImage image, java.nio.ByteBuffer rawBuffer,
                    long sequence, long timestampNs, int payloadSize) {
                check(image.getRGB(1, 2) == 0xff010203, "Pixel channel order");
                check(payloadSize == 64 * 48 * 4, "Pixel payload length");
                callbackCount.incrementAndGet();
            }
        }) {
            camera.open("mock", 64, 48, 30, RealtimeCamera.PIXEL_MODE_YUYV_TO_INT_RGB, -1);
            BufferedImage image = camera.capture(300, false);
            check(image != null, "Synchronous capture");
            check(image.getRGB(1, 2) == 0xff010203, "ARGB pixel value");
            check(camera.getPayloadBytesUsed() == 64 * 48 * 4, "Native payload length");
            check(camera.getFrameSequence() == 1, "Frame sequence");
            camera.scheduleCaptureAtFps(0, -1, 100);
            long deadlineNs = System.nanoTime() + 2_000_000_000L;
            while (callbackCount.get() < 5 && System.nanoTime() < deadlineNs) Thread.sleep(5);
            camera.stop();
            check(callbackCount.get() >= 5, "Scheduled callbacks");
            check(camera.getFrameSequence() >= 6, "Scheduled frame count");
        }
        System.out.println("PASS: portable mock camera capture and callback tests");
    }
}
