#include "../portable/camera_source.hpp"
#include "../portable/platform_support.hpp"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cmath>

namespace alcybe {
namespace {
template<typename T> void releaseCom(T*& pointer) {
    if (pointer) { pointer->Release(); pointer = nullptr; }
}
std::wstring utf8ToWide(const std::string& input) {
    int size = MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, nullptr, 0);
    if (size <= 0) return {};
    std::wstring result(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, input.c_str(), -1, result.data(), size);
    result.pop_back();
    return result;
}
}

class WindowsCameraSource final : public CameraSource {
public:
    WindowsCameraSource(int imageWidth, int imageHeight)
        : width(imageWidth), height(imageHeight) {}

    bool open(const std::string& deviceId, int targetFps, std::string& errorMessage) {
        HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(status)) { errorMessage = "Cannot initialize COM in the current thread."; return false; }
        const bool initializedCom = true;
        status = MFStartup(MF_VERSION, MFSTARTUP_NOSOCKET);
        if (FAILED(status)) {
            errorMessage = "Media Foundation startup failed.";
            CoUninitialize();
            return false;
        }
        startedMediaFoundation = true;
        IMFAttributes* attributes = nullptr;
        IMFActivate** devices = nullptr;
        UINT32 deviceCount = 0;
        status = MFCreateAttributes(&attributes, 1);
        if (SUCCEEDED(status)) status = attributes->SetGUID(
            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE, MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_GUID);
        if (SUCCEEDED(status)) status = MFEnumDeviceSources(attributes, &devices, &deviceCount);
        releaseCom(attributes);
        int deviceIndex = 0;
        if (deviceId != "default" && deviceId != "0") {
            char* parseEnd = nullptr;
            long parsed = std::strtol(deviceId.c_str(), &parseEnd, 10);
            if (parseEnd && *parseEnd == '\0' && parsed >= 0 && parsed < INT32_MAX) {
                deviceIndex = static_cast<int>(parsed);
            } else {
                std::wstring requestedId = utf8ToWide(deviceId);
                deviceIndex = -1;
                for (UINT32 index = 0; index < deviceCount; ++index) {
                    WCHAR* symbolicLink = nullptr;
                    UINT32 length = 0;
                    if (SUCCEEDED(devices[index]->GetAllocatedString(
                            MF_DEVSOURCE_ATTRIBUTE_SOURCE_TYPE_VIDCAP_SYMBOLIC_LINK,
                            &symbolicLink, &length)) && symbolicLink) {
                        if (requestedId == symbolicLink) deviceIndex = static_cast<int>(index);
                        CoTaskMemFree(symbolicLink);
                    }
                }
            }
        }
        if (SUCCEEDED(status) && deviceIndex >= 0 && static_cast<UINT32>(deviceIndex) < deviceCount)
            status = devices[deviceIndex]->ActivateObject(IID_PPV_ARGS(&source));
        else status = E_FAIL;
        if (devices) {
            for (UINT32 index = 0; index < deviceCount; ++index) devices[index]->Release();
            CoTaskMemFree(devices);
        }
        if (FAILED(status)) {
            errorMessage = "No matching Media Foundation camera is available.";
            CoUninitialize();
            return false;
        }
        IMFAttributes* readerAttributes = nullptr;
        status = MFCreateAttributes(&readerAttributes, 1);
        if (SUCCEEDED(status)) status = readerAttributes->SetUINT32(
            MF_SOURCE_READER_ENABLE_ADVANCED_VIDEO_PROCESSING, TRUE);
        if (SUCCEEDED(status)) status = MFCreateSourceReaderFromMediaSource(source, readerAttributes, &reader);
        releaseCom(readerAttributes);
        IMFMediaType* outputType = nullptr;
        if (SUCCEEDED(status)) status = MFCreateMediaType(&outputType);
        if (SUCCEEDED(status)) status = outputType->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        if (SUCCEEDED(status)) status = outputType->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        if (SUCCEEDED(status)) status = MFSetAttributeSize(outputType, MF_MT_FRAME_SIZE, width, height);
        if (SUCCEEDED(status)) status = MFSetAttributeRatio(outputType, MF_MT_FRAME_RATE, targetFps, 1);
        if (SUCCEEDED(status)) status = reader->SetCurrentMediaType(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outputType);
        releaseCom(outputType);
        if (SUCCEEDED(status)) {
            IMFMediaType* actualType = nullptr;
            status = reader->GetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, &actualType);
            UINT32 actualWidth = 0;
            UINT32 actualHeight = 0;
            if (SUCCEEDED(status)) status = MFGetAttributeSize(actualType, MF_MT_FRAME_SIZE, &actualWidth, &actualHeight);
            if (SUCCEEDED(status) && (actualWidth != UINT32(width) || actualHeight != UINT32(height))) status = E_FAIL;
            UINT32 rawStride = 0;
            if (SUCCEEDED(status) && SUCCEEDED(actualType->GetUINT32(MF_MT_DEFAULT_STRIDE, &rawStride)))
                strideBytes = static_cast<int32_t>(rawStride);
            releaseCom(actualType);
        }
        if (strideBytes == 0) strideBytes = width * 4;
        if (FAILED(status)) errorMessage = "Camera cannot provide the requested RGB32 size and frame rate.";
        if (initializedCom) CoUninitialize();
        return SUCCEEDED(status);
    }

    ~WindowsCameraSource() override {
        HRESULT status = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        releaseCom(reader);
        if (source) source->Shutdown();
        releaseCom(source);
        if (startedMediaFoundation) MFShutdown();
        if (SUCCEEDED(status)) CoUninitialize();
    }

    bool readFrame(uint32_t* destination, int timeoutMs, int64_t& timestampNs) override {
        // The synchronous Source Reader API does not provide a strict read timeout.
        (void)timeoutMs;
        HRESULT initialization = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(initialization)) return false;
        DWORD streamIndex = 0;
        DWORD flags = 0;
        LONGLONG sampleTime = 0;
        IMFSample* sample = nullptr;
        bool success = false;
        HRESULT status = reader->ReadSample(
            MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIndex, &flags, &sampleTime, &sample);
        if (SUCCEEDED(status) && sample && !(flags & MF_SOURCE_READERF_ENDOFSTREAM)) {
            IMFMediaBuffer* mediaBuffer = nullptr;
            status = sample->ConvertToContiguousBuffer(&mediaBuffer);
            if (SUCCEEDED(status)) {
                BYTE* bytes = nullptr;
                DWORD maxLength = 0;
                DWORD usedLength = 0;
                status = mediaBuffer->Lock(&bytes, &maxLength, &usedLength);
                const size_t rowBytes = size_t(width) * 4u;
                const size_t stride = size_t(strideBytes < 0 ? -int64_t(strideBytes) : strideBytes);
                if (SUCCEEDED(status) && bytes && stride >= rowBytes
                    && size_t(usedLength) >= stride * size_t(height)) {
                    for (int y = 0; y < height; ++y) {
                        size_t sourceRow = size_t(strideBytes < 0 ? height - 1 - y : y) * stride;
                        const uint8_t* row = bytes + sourceRow;
                        for (int x = 0; x < width; ++x) {
                            const uint8_t* pixel = row + size_t(x) * 4u;
                            destination[size_t(y) * width + x] = 0xff000000u
                                | (uint32_t(pixel[2]) << 16)
                                | (uint32_t(pixel[1]) << 8)
                                | uint32_t(pixel[0]);
                        }
                    }
                    timestampNs = nowNs();
                    success = true;
                }
                if (SUCCEEDED(status)) mediaBuffer->Unlock();
            }
            releaseCom(mediaBuffer);
        }
        releaseCom(sample);
        CoUninitialize();
        return success;
    }
private:
    int width;
    int height;
    int strideBytes = 0;
    IMFMediaSource* source = nullptr;
    IMFSourceReader* reader = nullptr;
    bool startedMediaFoundation = false;
};

std::unique_ptr<CameraSource> openCameraSource(
    const std::string& deviceId, int width, int height, int targetFps, std::string& errorMessage) {
    auto camera = std::make_unique<WindowsCameraSource>(width, height);
    if (!camera->open(deviceId, targetFps, errorMessage)) return nullptr;
    return camera;
}
}
