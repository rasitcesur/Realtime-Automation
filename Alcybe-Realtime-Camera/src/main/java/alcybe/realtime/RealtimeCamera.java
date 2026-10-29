package alcybe.realtime;

import java.awt.image.BufferedImage;
import java.awt.image.DataBufferInt;
import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.IntBuffer;
import javax.imageio.ImageIO;

/** Provides a shared camera API for Linux, macOS, and Windows. */
public class RealtimeCamera implements AutoCloseable {

    static {
        RealtimeInitializer.init();
    }

    // Pixel pipeline modes
    public static final int PIXEL_MODE_YUYV_TO_INT_RGB = 0; // Use the ARGB image pipeline.
    public static final int PIXEL_MODE_RAW_MJPEG       = 1; // Read compressed frames on Linux.

    // V4L2 Exposure Auto Modes (enum v4l2_exposure_auto_type)
    public static final int EXPOSURE_AUTO              = 0; // Automatic exposure time, automatic iris
    public static final int EXPOSURE_MANUAL            = 1; // Manual exposure time, manual iris
    public static final int EXPOSURE_SHUTTER_PRIORITY  = 2; // Manual exposure time, auto iris
    public static final int EXPOSURE_APERTURE_PRIORITY = 3; // Auto exposure time, manual iris (most common UVC Auto Mode)

    // V4L2 Power Line Frequency Anti-Flicker Modes
    public static final int POWER_LINE_FREQUENCY_DISABLED = 0;
    public static final int POWER_LINE_FREQUENCY_50HZ     = 1;
    public static final int POWER_LINE_FREQUENCY_60HZ     = 2;
    public static final int POWER_LINE_FREQUENCY_AUTO     = 3;

    // V4L2 Standard User Controls (Base 0x00980900)
    public static final int V4L2_CID_BRIGHTNESS              = 0x00980900;
    public static final int V4L2_CID_CONTRAST                = 0x00980901;
    public static final int V4L2_CID_SATURATION              = 0x00980902;
    public static final int V4L2_CID_HUE                     = 0x00980903;
    public static final int V4L2_CID_AUTO_WHITE_BALANCE      = 0x0098090c;
    public static final int V4L2_CID_RED_BALANCE             = 0x0098090e;
    public static final int V4L2_CID_BLUE_BALANCE            = 0x0098090f;
    public static final int V4L2_CID_GAMMA                   = 0x00980910;
    public static final int V4L2_CID_EXPOSURE                = 0x00980911;
    public static final int V4L2_CID_AUTOGAIN                = 0x00980912;
    public static final int V4L2_CID_GAIN                    = 0x00980913;
    public static final int V4L2_CID_HFLIP                   = 0x00980914;
    public static final int V4L2_CID_VFLIP                   = 0x00980915;
    public static final int V4L2_CID_POWER_LINE_FREQUENCY    = 0x00980918;
    public static final int V4L2_CID_HUE_AUTO                = 0x00980919;
    public static final int V4L2_CID_WHITE_BALANCE_TEMP      = 0x0098091a;
    public static final int V4L2_CID_SHARPNESS               = 0x0098091b;
    public static final int V4L2_CID_BACKLIGHT_COMPENSATION  = 0x0098091c;

    // V4L2 Camera Class Controls (Base 0x009a0900)
    public static final int V4L2_CID_EXPOSURE_AUTO           = 0x009a0901;
    public static final int V4L2_CID_EXPOSURE_ABSOLUTE       = 0x009a0902;
    public static final int V4L2_CID_EXPOSURE_AUTO_PRIORITY  = 0x009a0903;
    public static final int V4L2_CID_PAN_ABSOLUTE            = 0x009a0908;
    public static final int V4L2_CID_TILT_ABSOLUTE           = 0x009a0909;
    public static final int V4L2_CID_FOCUS_ABSOLUTE          = 0x009a090a;
    public static final int V4L2_CID_FOCUS_AUTO              = 0x009a090c;
    public static final int V4L2_CID_ZOOM_ABSOLUTE           = 0x009a090d;
    public static final int V4L2_CID_IRIS_ABSOLUTE           = 0x009a0911;
    public static final int V4L2_CID_ISO_SENSITIVITY         = 0x009a0917;
    public static final int V4L2_CID_ISO_SENSITIVITY_AUTO    = 0x009a0918;

    // 64-byte Cache-Aligned Shared Header Offsets
    public static final int OFFSET_FRAME_SEQUENCE    = 0;
    public static final int OFFSET_HW_TIMESTAMP_NS   = 8;
    public static final int OFFSET_POLL_TIMESTAMP_NS = 16;
    public static final int OFFSET_DUTY_CYCLE_NS     = 24;
    public static final int OFFSET_IDLE_CYCLE_NS     = 32;
    public static final int OFFSET_FRAME_INTERVAL_NS = 40;
    public static final int OFFSET_PAYLOAD_BYTES     = 48;
    public static final int OFFSET_CAMERA_STATE      = 52;
    public static final int OFFSET_DROPPED_FRAMES    = 56;
    public static final int OFFSET_GC_COUNT          = 60;
    public static final int OFFSET_FRAME_PAYLOAD     = 64;

    private static final VarHandle LONG_HANDLE = MethodHandles.byteBufferViewVarHandle(
            long[].class, ByteOrder.nativeOrder());
    private static final VarHandle INT_HANDLE = MethodHandles.byteBufferViewVarHandle(
            int[].class, ByteOrder.nativeOrder());

    private long camCtxPtr;
    private long sharedMemoryAddress;
    private ByteBuffer sharedDirectBuffer;
    private IntBuffer sharedPixelIntBuffer;

    // Reuse one image and raster array.
    private BufferedImage reusableBufferedImage;
    private int[] backingRasterArray;

    // Reuse the MJPEG input buffer.
    private byte[] reusableMjpegBuffer;
    private ExposedByteArrayInputStream reusableMjpegStream;

    // Reuse the control query buffer.
    private final int[] controlRangeScratch = new int[5];

    private int width;
    private int height;
    private int pixelMode;

    private static final class ExposedByteArrayInputStream extends ByteArrayInputStream {
        public ExposedByteArrayInputStream(byte[] buf) {
            super(buf);
        }
        public void resetView(int length) {
            this.pos = 0;
            this.count = length;
            this.mark = 0;
        }
    }

    /** Opens a camera and prepares reusable frame buffers. */
    public void open(String devicePath, int width, int height, int targetFps, int pixelMode, int numaNode) {
        if (camCtxPtr != 0) throw new IllegalStateException("Camera already open; close first");
        if (devicePath == null || devicePath.isBlank()) throw new IllegalArgumentException("Camera device path required");
        // Start GC coordination before capture.
        RealtimeThread.prepareIdleGcCoordinator();
        long ptr = openNativeCamera(devicePath, width, height, targetFps, pixelMode, numaNode);
        if (ptr == 0) throw new IllegalStateException("Cannot open or negotiate V4L2 camera: " + devicePath);
        try {
            ByteBuffer direct = getSharedFrameBuffer(ptr);
            if (direct == null) throw new OutOfMemoryError("Cannot create native camera view");
            direct = direct.order(ByteOrder.nativeOrder());
            ByteBuffer slice = direct.duplicate().order(ByteOrder.nativeOrder());
            slice.position(OFFSET_FRAME_PAYLOAD);
            IntBuffer pixels = slice.slice().order(ByteOrder.nativeOrder()).asIntBuffer();
            // Keep the image raster in Java memory.
            BufferedImage image = new BufferedImage(width, height, BufferedImage.TYPE_INT_RGB);
            int[] raster = ((DataBufferInt) image.getRaster().getDataBuffer()).getData();
            byte[] mjpeg = pixelMode == PIXEL_MODE_RAW_MJPEG
                    ? new byte[direct.capacity() - OFFSET_FRAME_PAYLOAD] : null;
            this.reusableMjpegBuffer = mjpeg;
            this.reusableMjpegStream = mjpeg == null ? null : new ExposedByteArrayInputStream(mjpeg);
            this.width = width;
            this.height = height;
            this.pixelMode = pixelMode;
            this.sharedDirectBuffer = direct;
            this.sharedPixelIntBuffer = pixels;
            this.reusableBufferedImage = image;
            this.backingRasterArray = raster;
            this.sharedMemoryAddress = getSharedFrameAddress(ptr);
            this.camCtxPtr = ptr;
        } catch (Throwable failure) {
            closeNativeCamera(ptr);
            throw failure;
        }
    }

    public void open(String devicePath, int width, int height) {
        open(devicePath, width, height, 60, PIXEL_MODE_YUYV_TO_INT_RGB, -1);
    }

    // 1. SINGLE CAPTURE API (YUYV reuses image; MJPEG decoding allocates)


    public BufferedImage capture() {
        return capture(100, true);
    }

    public BufferedImage capture(int timeoutMs, boolean triggerGcAfter) {
        if (camCtxPtr == 0) throw new IllegalStateException("Camera not open");
        int bytesUsed = captureSingleFrame(camCtxPtr, timeoutMs, triggerGcAfter);
        if (bytesUsed <= 0) {
            return null;
        }
        return updateBufferedImageFromSharedMemory(bytesUsed);
    }

    private BufferedImage updateBufferedImageFromSharedMemory(int payloadBytesUsed) {
        if (payloadBytesUsed < 1 || payloadBytesUsed > sharedDirectBuffer.capacity() - OFFSET_FRAME_PAYLOAD)
            throw new IllegalArgumentException("Invalid native frame size");
        if (pixelMode == PIXEL_MODE_YUYV_TO_INT_RGB) {
            if (payloadBytesUsed != backingRasterArray.length * Integer.BYTES)
                throw new IllegalArgumentException("Invalid YUYV conversion size");
            sharedPixelIntBuffer.position(0);
            sharedPixelIntBuffer.get(backingRasterArray, 0, backingRasterArray.length);
            return reusableBufferedImage;
        } else {
            ByteBuffer payloadView = sharedDirectBuffer.duplicate();
            payloadView.position(OFFSET_FRAME_PAYLOAD);
            payloadView.get(reusableMjpegBuffer, 0, payloadBytesUsed);
            reusableMjpegStream.resetView(payloadBytesUsed);
            try {
                return ImageIO.read(reusableMjpegStream);
            } catch (IOException e) {
                return null;
            }
        }
    }

    // 2. SCHEDULED CAPTURE API (Lite Soft-Realtime Scheduler)


    public void scheduleCapture(int priority, int cpuAffinity, long beginTimeNs, long timeOffsetNs, long cycleTimeNs) {
        if (camCtxPtr != 0) {
            startScheduledCapture(camCtxPtr, priority, cpuAffinity, beginTimeNs, timeOffsetNs, cycleTimeNs);
        }
    }

    public void scheduleCaptureAtFps(int priority, int cpuAffinity, int fps) {
        if (fps <= 0) throw new IllegalArgumentException("fps must be positive");
        long cycleTimeNs = 1_000_000_000L / fps;
        scheduleCapture(priority, cpuAffinity, 0L, 0L, cycleTimeNs);
    }

    private void onNativeFrameReady(long frameSequence, long hwTimestampNs, int payloadBytesUsed) {
        BufferedImage img = updateBufferedImageFromSharedMemory(payloadBytesUsed);
        if (img != null) {
            onFrameCaptured(img, sharedDirectBuffer, frameSequence, hwTimestampNs, payloadBytesUsed);
        }
    }

    private void onNativeIdleGcWindow(long availableIdleNs) {
        if (RealtimeThread.requestIdleGc(availableIdleNs) && sharedDirectBuffer != null) {
            // Count accepted GC requests.
            INT_HANDLE.getAndAdd(sharedDirectBuffer, OFFSET_GC_COUNT, 1);
        }
    }

    public void onFrameCaptured(
            BufferedImage image,
            ByteBuffer rawSharedBuffer,
            long frameSequence,
            long hwTimestampNs,
            int payloadBytesUsed) {
    }

    // 3. HARDWARE CAPABILITY VERIFICATION & SAFE V4L2 CONTROLS


    /** Checks whether a V4L2 control is supported. */
    public boolean isControlSupported(int controlId) {
        return camCtxPtr != 0 && isV4L2ControlSupported(camCtxPtr, controlId);
    }

    /** Checks whether a V4L2 menu value is supported. */
    public boolean isMenuValueSupported(int controlId, int menuValue) {
        return camCtxPtr != 0 && isV4L2MenuValueSupported(camCtxPtr, controlId, menuValue);
    }

    /** Sets a supported V4L2 control. */
    private boolean safeSetControl(int controlId, int value) {
        if (camCtxPtr == 0) return false;
        if (!isV4L2ControlSupported(camCtxPtr, controlId)) {
            return false;
        }
        return setV4L2Control(camCtxPtr, controlId, value);
    }

    /** Configures automatic exposure when available. */
    public boolean setAutoExposureEnabled(boolean enabled, boolean allowVariableFramerate) {
        if (camCtxPtr == 0) return false;

        // Check the exposure control.
        if (!isV4L2ControlSupported(camCtxPtr, V4L2_CID_EXPOSURE_AUTO)) {
            return false;
        }

        boolean modeApplied = false;
        if (enabled) {
            // Check automatic exposure modes.
            if (isV4L2MenuValueSupported(camCtxPtr, V4L2_CID_EXPOSURE_AUTO, EXPOSURE_APERTURE_PRIORITY)) {
                modeApplied = setV4L2Control(camCtxPtr, V4L2_CID_EXPOSURE_AUTO, EXPOSURE_APERTURE_PRIORITY);
            } else if (isV4L2MenuValueSupported(camCtxPtr, V4L2_CID_EXPOSURE_AUTO, EXPOSURE_AUTO)) {
                modeApplied = setV4L2Control(camCtxPtr, V4L2_CID_EXPOSURE_AUTO, EXPOSURE_AUTO);
            }
            // Set auto priority when available.
            if (isV4L2ControlSupported(camCtxPtr, V4L2_CID_EXPOSURE_AUTO_PRIORITY)) {
                setV4L2Control(camCtxPtr, V4L2_CID_EXPOSURE_AUTO_PRIORITY, allowVariableFramerate ? 1 : 0);
            }
        } else {
            // Check manual exposure modes.
            if (isV4L2MenuValueSupported(camCtxPtr, V4L2_CID_EXPOSURE_AUTO, EXPOSURE_MANUAL)) {
                modeApplied = setV4L2Control(camCtxPtr, V4L2_CID_EXPOSURE_AUTO, EXPOSURE_MANUAL);
            } else if (isV4L2MenuValueSupported(camCtxPtr, V4L2_CID_EXPOSURE_AUTO, EXPOSURE_SHUTTER_PRIORITY)) {
                modeApplied = setV4L2Control(camCtxPtr, V4L2_CID_EXPOSURE_AUTO, EXPOSURE_SHUTTER_PRIORITY);
            }
            if (isV4L2ControlSupported(camCtxPtr, V4L2_CID_EXPOSURE_AUTO_PRIORITY)) {
                setV4L2Control(camCtxPtr, V4L2_CID_EXPOSURE_AUTO_PRIORITY, 0);
            }
        }
        return modeApplied;
    }

    public boolean setAutoExposureMode(int mode) {
        if (!isMenuValueSupported(V4L2_CID_EXPOSURE_AUTO, mode)) {
            return false;
        }
        return safeSetControl(V4L2_CID_EXPOSURE_AUTO, mode);
    }

    public int getAutoExposureMode() {
        return getV4L2Control(camCtxPtr, V4L2_CID_EXPOSURE_AUTO);
    }

    public boolean setExposureAutoPriority(boolean allowFrameRateDrop) {
        return safeSetControl(V4L2_CID_EXPOSURE_AUTO_PRIORITY, allowFrameRateDrop ? 1 : 0);
    }

    /** Sets a fixed exposure time when available. */
    public boolean setFixedExposureTime(int exposureValue100Us) {
        setAutoExposureEnabled(false, false);
        if (safeSetControl(V4L2_CID_EXPOSURE_ABSOLUTE, exposureValue100Us)) {
            return true;
        }
        return safeSetControl(V4L2_CID_EXPOSURE, exposureValue100Us);
    }

    /** Sets the exposure value when the control is active. */
    public boolean setExposureTime(int exposureValue100Us) {
        if (safeSetControl(V4L2_CID_EXPOSURE_ABSOLUTE, exposureValue100Us)) {
            return true;
        }
        return safeSetControl(V4L2_CID_EXPOSURE, exposureValue100Us);
    }

    public int getExposureTime() {
        int val = getV4L2Control(camCtxPtr, V4L2_CID_EXPOSURE_ABSOLUTE);
        return (val >= 0) ? val : getV4L2Control(camCtxPtr, V4L2_CID_EXPOSURE);
    }

    public boolean setBrightness(int value) {
        return safeSetControl(V4L2_CID_BRIGHTNESS, value);
    }

    public int getBrightness() {
        return getV4L2Control(camCtxPtr, V4L2_CID_BRIGHTNESS);
    }

    public boolean setContrast(int value) {
        return safeSetControl(V4L2_CID_CONTRAST, value);
    }

    public int getContrast() {
        return getV4L2Control(camCtxPtr, V4L2_CID_CONTRAST);
    }

    public boolean setSaturation(int value) {
        return safeSetControl(V4L2_CID_SATURATION, value);
    }

    public int getSaturation() {
        return getV4L2Control(camCtxPtr, V4L2_CID_SATURATION);
    }

    public boolean setHue(int value) {
        return safeSetControl(V4L2_CID_HUE, value);
    }

    public int getHue() {
        return getV4L2Control(camCtxPtr, V4L2_CID_HUE);
    }

    public boolean setAutoHue(boolean enabled) {
        return safeSetControl(V4L2_CID_HUE_AUTO, enabled ? 1 : 0);
    }

    public boolean setSharpness(int value) {
        return safeSetControl(V4L2_CID_SHARPNESS, value);
    }

    public int getSharpness() {
        return getV4L2Control(camCtxPtr, V4L2_CID_SHARPNESS);
    }

    public boolean setGamma(int value) {
        return safeSetControl(V4L2_CID_GAMMA, value);
    }

    public int getGamma() {
        return getV4L2Control(camCtxPtr, V4L2_CID_GAMMA);
    }

    public boolean setGain(int value) {
        return safeSetControl(V4L2_CID_GAIN, value);
    }

    public int getGain() {
        return getV4L2Control(camCtxPtr, V4L2_CID_GAIN);
    }

    public boolean setAutoGain(boolean enabled) {
        return safeSetControl(V4L2_CID_AUTOGAIN, enabled ? 1 : 0);
    }

    public boolean setAutoWhiteBalance(boolean enabled) {
        return safeSetControl(V4L2_CID_AUTO_WHITE_BALANCE, enabled ? 1 : 0);
    }

    public boolean setWhiteBalanceTemperature(int kelvin) {
        safeSetControl(V4L2_CID_AUTO_WHITE_BALANCE, 0);
        return safeSetControl(V4L2_CID_WHITE_BALANCE_TEMP, kelvin);
    }

    public int getWhiteBalanceTemperature() {
        return getV4L2Control(camCtxPtr, V4L2_CID_WHITE_BALANCE_TEMP);
    }

    public boolean setRedBalance(int value) {
        return safeSetControl(V4L2_CID_RED_BALANCE, value);
    }

    public boolean setBlueBalance(int value) {
        return safeSetControl(V4L2_CID_BLUE_BALANCE, value);
    }

    public boolean setBacklightCompensation(int value) {
        return safeSetControl(V4L2_CID_BACKLIGHT_COMPENSATION, value);
    }

    public boolean setPowerLineFrequency(int freqMode) {
        if (!isMenuValueSupported(V4L2_CID_POWER_LINE_FREQUENCY, freqMode)) {
            return false;
        }
        return safeSetControl(V4L2_CID_POWER_LINE_FREQUENCY, freqMode);
    }

    public boolean setAutoFocus(boolean enabled) {
        return safeSetControl(V4L2_CID_FOCUS_AUTO, enabled ? 1 : 0);
    }

    public boolean setFocusAbsolute(int focusValue) {
        safeSetControl(V4L2_CID_FOCUS_AUTO, 0);
        return safeSetControl(V4L2_CID_FOCUS_ABSOLUTE, focusValue);
    }

    public int getFocusAbsolute() {
        return getV4L2Control(camCtxPtr, V4L2_CID_FOCUS_ABSOLUTE);
    }

    public boolean setZoomAbsolute(int zoomValue) {
        return safeSetControl(V4L2_CID_ZOOM_ABSOLUTE, zoomValue);
    }

    public boolean setPanAbsolute(int panValue) {
        return safeSetControl(V4L2_CID_PAN_ABSOLUTE, panValue);
    }

    public boolean setTiltAbsolute(int tiltValue) {
        return safeSetControl(V4L2_CID_TILT_ABSOLUTE, tiltValue);
    }

    public boolean setHorizontalFlip(boolean flip) {
        return safeSetControl(V4L2_CID_HFLIP, flip ? 1 : 0);
    }

    public boolean setVerticalFlip(boolean flip) {
        return safeSetControl(V4L2_CID_VFLIP, flip ? 1 : 0);
    }

    public boolean setIsoSensitivity(int isoValue) {
        safeSetControl(V4L2_CID_ISO_SENSITIVITY_AUTO, 0);
        return safeSetControl(V4L2_CID_ISO_SENSITIVITY, isoValue);
    }

    public boolean setCustomControl(int controlId, int value) {
        return safeSetControl(controlId, value);
    }

    public int getCustomControl(int controlId) {
        return getV4L2Control(camCtxPtr, controlId);
    }

    /** Reads the supported range of a V4L2 control. */
    public synchronized boolean queryControlLimits(int controlId, int[] outMinMaxStepDefault) {
        if (camCtxPtr == 0 || outMinMaxStepDefault == null || outMinMaxStepDefault.length < 4) {
            return false;
        }
        return queryV4L2ControlRange(camCtxPtr, controlId, outMinMaxStepDefault);
    }

    public synchronized boolean resetControlToDefault(int controlId) {
        if (!isControlSupported(controlId)) {
            return false;
        }
        if (queryV4L2ControlRange(camCtxPtr, controlId, controlRangeScratch)) {
            int defaultVal = controlRangeScratch[3];
            return safeSetControl(controlId, defaultVal);
        }
        return false;
    }

    // 4. TELEMETRY & LIFECYCLE ACCESSORS


    private ByteBuffer buffer() {
        if (camCtxPtr == 0 || sharedDirectBuffer == null) throw new IllegalStateException("Camera is closed");
        return sharedDirectBuffer;
    }

    public long getFrameSequence() {
        return (long) LONG_HANDLE.getVolatile(buffer(), OFFSET_FRAME_SEQUENCE);
    }

    public long getHardwareTimestampNanos() {
        return (long) LONG_HANDLE.getVolatile(buffer(), OFFSET_HW_TIMESTAMP_NS);
    }

    public long getPollTimestampNanos() {
        return (long) LONG_HANDLE.getVolatile(buffer(), OFFSET_POLL_TIMESTAMP_NS);
    }

    public long getDutyCycleNanos() {
        return (long) LONG_HANDLE.getVolatile(buffer(), OFFSET_DUTY_CYCLE_NS);
    }

    public long getIdleCycleNanos() {
        return (long) LONG_HANDLE.getVolatile(buffer(), OFFSET_IDLE_CYCLE_NS);
    }

    public long getFrameIntervalNanos() {
        return (long) LONG_HANDLE.getVolatile(buffer(), OFFSET_FRAME_INTERVAL_NS);
    }

    public int getPayloadBytesUsed() {
        return (int) INT_HANDLE.getVolatile(buffer(), OFFSET_PAYLOAD_BYTES);
    }

    public int getDroppedFramesCount() {
        return (int) INT_HANDLE.getVolatile(buffer(), OFFSET_DROPPED_FRAMES);
    }

    public int getGcTriggeredCount() {
        return (int) INT_HANDLE.getVolatile(buffer(), OFFSET_GC_COUNT);
    }

    public int readPixelArgb(int x, int y) {
        if (pixelMode != PIXEL_MODE_YUYV_TO_INT_RGB) throw new IllegalStateException("Pixel reads require YUYV mode");
        if (x < 0 || y < 0 || x >= width || y >= height) throw new IndexOutOfBoundsException("Pixel coordinates out of range");
        int pixelOffset = OFFSET_FRAME_PAYLOAD + ((y * width + x) << 2);
        return (int) INT_HANDLE.getAcquire(buffer(), pixelOffset);
    }

    public long getRawSharedAddress() {
        buffer();
        return sharedMemoryAddress;
    }

    public BufferedImage getReusableBufferedImage() {
        return reusableBufferedImage;
    }

    public void pause() {
        if (camCtxPtr != 0) pauseCapture(camCtxPtr);
    }

    public void resume() {
        if (camCtxPtr != 0) resumeCapture(camCtxPtr);
    }

    public void stop() {
        if (camCtxPtr != 0) stopCapture(camCtxPtr);
    }

    @Override
    public void close() {
        if (camCtxPtr != 0) {
            closeNativeCamera(camCtxPtr);
            camCtxPtr = 0;
            sharedMemoryAddress = 0;
            sharedDirectBuffer = null;
            sharedPixelIntBuffer = null;
            reusableBufferedImage = null;
            backingRasterArray = null;
            reusableMjpegBuffer = null;
            reusableMjpegStream = null;
        }
    }

    // Native entry points.
    private native long openNativeCamera(String devicePath, int width, int height, int targetFps, int pixelMode, int numaNode);
    private native ByteBuffer getSharedFrameBuffer(long camCtxPtr);
    private native long getSharedFrameAddress(long camCtxPtr);
    private native int captureSingleFrame(long camCtxPtr, int timeoutMs, boolean triggerGcAfter);
    private native void startScheduledCapture(long camCtxPtr, int priority, int cpuAffinity, long beginTimeNs, long offsetNs, long cycleTimeNs);
    private native void pauseCapture(long camCtxPtr);
    private native void resumeCapture(long camCtxPtr);
    private native void stopCapture(long camCtxPtr);
    private native void closeNativeCamera(long camCtxPtr);
    private native boolean isV4L2ControlSupported(long camCtxPtr, int controlId);
    private native boolean isV4L2MenuValueSupported(long camCtxPtr, int controlId, int menuIndex);
    private native boolean setV4L2Control(long camCtxPtr, int controlId, int value);
    private native int getV4L2Control(long camCtxPtr, int controlId);
    private native boolean queryV4L2ControlRange(long camCtxPtr, int controlId, int[] outRangeInfo);
}