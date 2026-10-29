#import <AVFoundation/AVFoundation.h>
#import <CoreVideo/CoreVideo.h>
#import <Foundation/Foundation.h>
#import <dispatch/dispatch.h>
#include "../portable/camera_source.hpp"
#include "../portable/platform_support.hpp"
#include <condition_variable>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>

namespace alcybe { class MacCameraSource; }

@interface CameraFrameDelegate : NSObject <AVCaptureVideoDataOutputSampleBufferDelegate> {
@public
    alcybe::MacCameraSource* owner;
}
@end

namespace alcybe {
class MacCameraSource final : public CameraSource {
public:
    MacCameraSource(int imageWidth, int imageHeight)
        : width(imageWidth), height(imageHeight), latestPixels(size_t(imageWidth) * imageHeight) {}

    bool open(const std::string& deviceId, int targetFps, std::string& errorMessage) {
        @autoreleasepool {
            AVAuthorizationStatus access = [AVCaptureDevice authorizationStatusForMediaType:AVMediaTypeVideo];
            if (access == AVAuthorizationStatusNotDetermined) {
                dispatch_semaphore_t permissionReady = dispatch_semaphore_create(0);
                __block BOOL granted = NO;
                [AVCaptureDevice requestAccessForMediaType:AVMediaTypeVideo completionHandler:^(BOOL allowed) {
                    granted = allowed;
                    dispatch_semaphore_signal(permissionReady);
                }];
                dispatch_semaphore_wait(permissionReady, DISPATCH_TIME_FOREVER);
                access = granted ? AVAuthorizationStatusAuthorized : AVAuthorizationStatusDenied;
            }
            if (access != AVAuthorizationStatusAuthorized) {
                errorMessage = "Camera access is denied. Check macOS Privacy & Security settings.";
                return false;
            }
            NSArray<AVCaptureDevice*>* devices = [AVCaptureDevice devicesWithMediaType:AVMediaTypeVideo];
            AVCaptureDevice* device = nil;
            if (deviceId == "default" || deviceId == "0") {
                device = devices.firstObject;
            } else {
                for (AVCaptureDevice* candidate in devices) {
                    if (deviceId == std::string(candidate.uniqueID.UTF8String)) {
                        device = candidate;
                        break;
                    }
                }
            }
            if (!device) { errorMessage = "Camera device was not found."; return false; }
            NSError* error = nil;
            AVCaptureDeviceInput* input = [AVCaptureDeviceInput deviceInputWithDevice:device error:&error];
            if (!input) { errorMessage = "Cannot open the selected camera."; return false; }
            session = [[AVCaptureSession alloc] init];
            [session beginConfiguration];
            NSString* preset = nil;
            if (width == 640 && height == 480) preset = AVCaptureSessionPreset640x480;
            else if (width == 1280 && height == 720) preset = AVCaptureSessionPreset1280x720;
            else if (width == 1920 && height == 1080) preset = AVCaptureSessionPreset1920x1080;
            else if (width == 352 && height == 288) preset = AVCaptureSessionPreset352x288;
            if (!preset || ![session canSetSessionPreset:preset]) {
                [session commitConfiguration];
                errorMessage = "This macOS backend supports 352x288, 640x480, 1280x720 or 1920x1080 when available.";
                return false;
            }
            session.sessionPreset = preset;
            if (![session canAddInput:input]) {
                [session commitConfiguration];
                errorMessage = "Cannot add camera input.";
                return false;
            }
            [session addInput:input];
            AVCaptureVideoDataOutput* output = [[AVCaptureVideoDataOutput alloc] init];
            output.videoSettings = @{(id)kCVPixelBufferPixelFormatTypeKey: @(kCVPixelFormatType_32BGRA)};
            output.alwaysDiscardsLateVideoFrames = YES;
            if (![session canAddOutput:output]) {
                [session commitConfiguration];
                errorMessage = "Cannot add camera output.";
                return false;
            }
            [session addOutput:output];
            delegate = [[CameraFrameDelegate alloc] init];
            delegate->owner = this;
            captureQueue = dispatch_queue_create("alcybe.camera.capture", DISPATCH_QUEUE_SERIAL);
            [output setSampleBufferDelegate:delegate queue:captureQueue];
            videoOutput = output;
            if (targetFps > 0 && [device lockForConfiguration:&error]) {
                CMTime frameDuration = CMTimeMake(1, targetFps);
                for (AVFrameRateRange* frameRange in device.activeFormat.videoSupportedFrameRateRanges) {
                    if (frameRange.minFrameRate <= targetFps && targetFps <= frameRange.maxFrameRate) {
                        device.activeVideoMinFrameDuration = frameDuration;
                        device.activeVideoMaxFrameDuration = frameDuration;
                        break;
                    }
                }
                [device unlockForConfiguration];
            }
            [session commitConfiguration];
            [session startRunning];
            if (!session.running) { errorMessage = "Camera session did not start."; return false; }
            return true;
        }
    }

    ~MacCameraSource() override {
        @autoreleasepool {
            if (videoOutput) [videoOutput setSampleBufferDelegate:nil queue:nullptr];
            if (session) [session stopRunning];
            if (captureQueue) dispatch_sync(captureQueue, ^{});
            if (delegate) delegate->owner = nullptr;
        }
    }

    void acceptFrame(CMSampleBufferRef sampleBuffer) {
        CVImageBufferRef image = CMSampleBufferGetImageBuffer(sampleBuffer);
        if (!image || CVPixelBufferGetWidth(image) != size_t(width)
            || CVPixelBufferGetHeight(image) != size_t(height)
            || CVPixelBufferGetPixelFormatType(image) != kCVPixelFormatType_32BGRA) return;
        if (CVPixelBufferLockBaseAddress(image, kCVPixelBufferLock_ReadOnly) != kCVReturnSuccess) return;
        auto* source = static_cast<const uint8_t*>(CVPixelBufferGetBaseAddress(image));
        size_t stride = CVPixelBufferGetBytesPerRow(image);
        if (source && stride >= size_t(width) * 4u) {
            std::lock_guard<std::mutex> lock(frameMutex);
            for (int y = 0; y < height; ++y) {
                const uint8_t* sourceRow = source + size_t(y) * stride;
                uint32_t* targetRow = latestPixels.data() + size_t(y) * width;
                for (int x = 0; x < width; ++x) {
                    const uint8_t* pixel = sourceRow + size_t(x) * 4u;
                    targetRow[x] = 0xff000000u | (uint32_t(pixel[2]) << 16)
                                 | (uint32_t(pixel[1]) << 8) | uint32_t(pixel[0]);
                }
            }
            timestampNs = nowNs();
            ++generation;
            frameChanged.notify_all();
        }
        CVPixelBufferUnlockBaseAddress(image, kCVPixelBufferLock_ReadOnly);
    }

    bool readFrame(uint32_t* destination, int timeoutMs, int64_t& outputTimestampNs) override {
        std::unique_lock<std::mutex> lock(frameMutex);
        uint64_t previousGeneration = deliveredGeneration;
        if (!frameChanged.wait_for(lock, std::chrono::milliseconds(timeoutMs),
                [&] { return generation != previousGeneration; })) return false;
        std::memcpy(destination, latestPixels.data(), latestPixels.size() * sizeof(uint32_t));
        deliveredGeneration = generation;
        outputTimestampNs = timestampNs;
        return true;
    }
private:
    const int width;
    const int height;
    std::mutex frameMutex;
    std::condition_variable frameChanged;
    std::vector<uint32_t> latestPixels;
    uint64_t generation = 0;
    uint64_t deliveredGeneration = 0;
    int64_t timestampNs = 0;
    __strong AVCaptureSession* session = nil;
    __strong AVCaptureVideoDataOutput* videoOutput = nil;
    __strong CameraFrameDelegate* delegate = nil;
    dispatch_queue_t captureQueue = nullptr;
};

std::unique_ptr<CameraSource> openCameraSource(
    const std::string& deviceId, int width, int height, int targetFps, std::string& errorMessage) {
    auto source = std::make_unique<MacCameraSource>(width, height);
    if (!source->open(deviceId, targetFps, errorMessage)) return nullptr;
    return source;
}
}

@implementation CameraFrameDelegate
- (void)captureOutput:(AVCaptureOutput*)output didOutputSampleBuffer:(CMSampleBufferRef)sampleBuffer
       fromConnection:(AVCaptureConnection*)connection {
    (void)output;
    (void)connection;
    if (owner) owner->acceptFrame(sampleBuffer);
}
@end
