# Changelog

## v0.1.0

- Added a shared Java API and C++17/JNI core for desktop platforms.
- Kept Linux V4L2 capture, NUMA, thread scheduling, and camera controls.
- Added an AVFoundation capture backend for macOS.
- Added a Media Foundation capture backend for Windows.
- Added platform build scripts, CMake, and GitHub Actions.
- Added a mock camera test for the portable native core.
- Kept the 1 ms idle-GC trigger and the 50 ms request interval.

This is an experimental release. Native camera and scheduling behavior must be tested on each target system.
