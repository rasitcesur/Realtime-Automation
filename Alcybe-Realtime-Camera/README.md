# Alcybe Real-Time Camera

A Java 21+ and C++17/JNI camera runtime for Linux, macOS, and Windows.

**Release:** v0.1.0 · **License:** MIT · **Status:** experimental

The Java API is shared across platforms. Native capture and thread policies use the host operating system. The library is a soft real-time system. It does not guarantee a 1 ms capture or scheduling deadline.

## Platform support

| Platform | Camera backend | Thread policy | Library | Validation |
| --- | --- | --- | --- | --- |
| Linux | V4L2, MMAP | pthread, SCHED_FIFO, CPU affinity | `librealtime.so` | Build and automated JNI tests passed on Linux |
| macOS | AVFoundation, BGRA | POSIX threads, user-interactive QoS | `librealtime.dylib` | Source provided. macOS build and camera tests pending |
| Windows | Media Foundation, RGB32 | Win32, MMCSS, CPU affinity | `realtime.dll` | Source provided. Windows build and camera tests pending |

Windows uses `VirtualAllocExNuma` when a valid NUMA node is requested. Linux uses `libnuma` if it is installed at build time. macOS has no equivalent NUMA binding API in this project.

Linux's `SCHED_FIFO` is not equivalent to Windows MMCSS or macOS QoS. CPU pinning is not supported on macOS. Unsupported settings fail with a clear error.

## Features

- `RealtimeCamera` provides single-frame capture and scheduled callbacks.
- `RealtimeThread` provides periodic native workers and a shared memory block.
- The shared camera header is 64 bytes. It stores timestamps, frame counts, duty time, idle time, and dropped-frame counts.
- The default pixel mode returns packed ARGB pixels to a reusable `BufferedImage`.
- Linux also supports raw MJPEG and V4L2 hardware controls.
- macOS and Windows use native BGRA or RGB32 capture and convert to ARGB. Raw MJPEG and V4L2 controls are not supported there.
- The idle-GC policy checks for more than 1 ms of estimated idle time. It limits accepted requests to one every 50 ms. A separate Java daemon calls `System.gc()`.
- The Java API and the shared memory field offsets remain compatible with the Linux implementation.

The 1 ms GC value is an *idle-time trigger*, not a strict upper bound on a GC pause. ZGC executes most GC work concurrently. Measure actual STW pauses and frame jitter on the target machine.

## Requirements

- JDK 21 or later, with `JAVA_HOME` configured.
- CMake 3.20 or later.
- Linux: GCC 10+, V4L2 headers, and optional `libnuma-dev`.
- macOS: Xcode Command Line Tools and Apple Clang with Objective-C++ support.
- Windows: MSYS2 UCRT64 with MinGW-w64 GCC, CMake, and Ninja.

The scripts run Java using `-XX:+UseZGC`. Java 21 can also enable Generational ZGC with `-XX:+ZGenerational`. Do not use that flag on JDK 24 or later.

## Linux

```bash
./scripts/build-linux.sh
./scripts/test-linux.sh
./scripts/run-camera-linux.sh /dev/video0 640 480 30 100 YUYV
```

Check the device's supported frame formats with `v4l2-ctl --list-formats-ext -d /dev/video0`.

## macOS (Apple Silicon and Intel)

```bash
export JAVA_HOME=$(/usr/libexec/java_home -v 21)
./scripts/build-macos.sh
./scripts/test-macos.sh
./scripts/run-camera-macos.sh default 640 480 30 100
```

Use `default`, `0`, or the camera's AVFoundation unique ID. Grant camera access when macOS asks. Packaged applications need the `NSCameraUsageDescription` key shown in `platform/macos/Info.plist`.

AVFoundation produces BGRA frames. The native code stores ARGB values in the shared buffer. This is a copy-based image pipeline.

## Windows (PowerShell, MSYS2 UCRT64)

Install `mingw-w64-ucrt-x86_64-gcc`, `mingw-w64-ucrt-x86_64-cmake`, and `mingw-w64-ucrt-x86_64-ninja`. Add `C:\msys64\ucrt64\bin` to `PATH` before invoking the scripts.

```powershell
$env:JAVA_HOME = "C:\Program Files\Eclipse Adoptium\jdk-21"
$env:PATH = "C:\msys64\ucrt64\bin;" + $env:PATH
.\scripts\build-windows.ps1
.\scripts\test-windows.ps1
.\scripts\run-camera-windows.ps1 -deviceId 0 -width 640 -height 480 -fps 30
```

Use `default`, a zero-based camera index, or a Media Foundation symbolic link. The Source Reader uses RGB32 conversion. It may block during `ReadSample()`. The `timeoutMs` parameter is not a strict timeout on Windows.

## Build with CMake

You can use CMake directly after setting `JAVA_HOME`:

```bash
cmake -S . -B build/cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build/cmake --parallel
```

The Java source is compiled by the platform scripts. Native outputs are written under `build/native/`.

## Portable tests without a camera

The mock backend tests the same portable JNI core used on macOS and Windows. It runs on Linux without camera hardware.

```bash
./scripts/test-portable-linux.sh
```

The Linux V4L2 tests and mock backend tests use separate JVM processes and separate native output directories.

## Repository structure

```text
native/
  alcybe_realtime_RealtimeCamera.cpp   # Linux V4L2 capture
  alcybe_realtime_RealtimeThread.cpp   # Linux thread runtime
  portable/                            # Shared macOS/Windows JNI runtime
  macos/                               # AVFoundation adapter
  windows/                             # Media Foundation adapter
src/main/java/alcybe/realtime/         # Public Java API
src/test/java/alcybe/realtime/         # JNI and API tests
tests/native/                         # Pixel and mock camera tests
scripts/                              # Build, test, and run scripts
platform/macos/                       # Camera permission example
.github/workflows/                    # Build and test automation
```

## Limitations

The full camera path is not zero-copy. Native frames are copied into an off-heap buffer and then into a reusable Java raster. The shared buffer must not be used after closing the camera.

A real camera is required to validate device negotiation, timestamp behavior, CPU load, dropped frames, and p99/p99.9 latency. macOS and Windows binaries are not validated by the Linux test environment. GitHub Actions includes native build and no-camera smoke-test jobs for both platforms.

See `docs/PLATFORM_ARCHITECTURE.md`, `docs/CAMERA_API.md`, and `docs/VALIDATION.md` for details.
