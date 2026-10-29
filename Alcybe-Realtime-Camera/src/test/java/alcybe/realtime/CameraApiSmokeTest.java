package alcybe.realtime;

/** Hardware-independent JNI validation and camera API smoke tests. */
public final class CameraApiSmokeTest {
    private static int checks;
    private static void check(boolean actual, String description) {
        if (!actual) throw new AssertionError(description);
        checks++;
    }
    private static void expect(Class<? extends Throwable> kind, Runnable fn, String context) {
        try {
            fn.run();
        } catch (Throwable caught) {
            if (kind.isInstance(caught)) { checks++; return; }
            throw new AssertionError(context + " threw " + caught, caught);
        }
        throw new AssertionError(context + " should throw " + kind.getName());
    }
    public static void main(String[] ignored) {
        RealtimeCamera c = new RealtimeCamera();
        check(!c.isControlSupported(RealtimeCamera.V4L2_CID_BRIGHTNESS), "closed camera control check");
        expect(IllegalStateException.class, c::capture, "capture before open");
        expect(IllegalStateException.class, c::getFrameSequence, "telemetry before open");
        expect(IllegalStateException.class, c::getRawSharedAddress, "address before open");
        expect(IllegalArgumentException.class, () -> c.open("", 640, 480), "empty device path");
        expect(IllegalArgumentException.class, () -> c.open("/dev/null", 641, 480, 30, 0, -1), "odd YUYV width");
        expect(IllegalArgumentException.class, () -> c.open("/dev/null", 640, 480, 0, 0, -1), "invalid FPS");
        expect(IllegalArgumentException.class, () -> c.open("/dev/null", 640, 480, 30, 2, -1), "invalid pixel mode");
        expect(IllegalArgumentException.class, () -> c.open("/dev/null", 640, 480, 30, 0, -2), "invalid NUMA node");
        expect(IllegalArgumentException.class, () -> c.scheduleCaptureAtFps(0, -1, 0), "invalid scheduled FPS");
        expect(IllegalStateException.class, () -> c.open("/dev/null", 640, 480), "non-video device");
        check(!c.queryControlLimits(RealtimeCamera.V4L2_CID_BRIGHTNESS, new int[4]), "closed control query");
        check(c.getCustomControl(RealtimeCamera.V4L2_CID_BRIGHTNESS) == -1, "closed control get");
        check(!c.setCustomControl(RealtimeCamera.V4L2_CID_BRIGHTNESS, 4), "closed control set");
        c.close(); c.close();
        // Shared, off-worker GC dispatcher is also used by camera callbacks.
        RealtimeThread.prepareIdleGcCoordinator();
        check(!RealtimeThread.requestIdleGc(999_999L), "below-threshold idle GC request");
        System.out.println("PASS: " + checks + " camera API smoke checks (no physical device required)");
    }
}
