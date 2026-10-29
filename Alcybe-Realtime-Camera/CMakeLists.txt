cmake_minimum_required(VERSION 3.20)
project(AlcybeRealtimeCamera LANGUAGES CXX)

option(ALCYBE_PORTABLE_TEST_BACKEND "Build the portable native core with a mock camera" OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
    set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/native")
endif()
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_SOURCE_DIR}/build/native")
endif()

if(NOT DEFINED ENV{JAVA_HOME})
    message(FATAL_ERROR "Set JAVA_HOME to a JDK 21 or later installation")
endif()
file(TO_CMAKE_PATH "$ENV{JAVA_HOME}" javaHome)
set(javaHeaders "${javaHome}/include")
if(WIN32)
    set(javaPlatformHeaders "${javaHeaders}/win32")
elseif(APPLE)
    set(javaPlatformHeaders "${javaHeaders}/darwin")
else()
    set(javaPlatformHeaders "${javaHeaders}/linux")
endif()
if(NOT EXISTS "${javaHeaders}/jni.h" OR NOT EXISTS "${javaPlatformHeaders}/jni_md.h")
    message(FATAL_ERROR "JNI headers were not found under JAVA_HOME")
endif()

if(ALCYBE_PORTABLE_TEST_BACKEND)
    add_library(realtime SHARED
        native/portable/realtime_thread.cpp
        native/portable/realtime_camera.cpp
        tests/native/mock_camera_source.cpp)
elseif(APPLE)
    enable_language(OBJCXX)
    add_library(realtime SHARED
        native/portable/realtime_thread.cpp
        native/portable/realtime_camera.cpp
        native/macos/camera_avfoundation.mm)
    set_source_files_properties(native/macos/camera_avfoundation.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc;-fblocks")
    target_link_libraries(realtime PRIVATE
        "-framework AVFoundation" "-framework CoreMedia" "-framework CoreVideo" "-framework Foundation")
elseif(WIN32)
    add_library(realtime SHARED
        native/portable/realtime_thread.cpp
        native/portable/realtime_camera.cpp
        native/windows/camera_media_foundation.cpp)
    target_link_libraries(realtime PRIVATE mfplat mfreadwrite mfuuid mf ole32 avrt)
else()
    add_library(realtime SHARED
        native/alcybe_realtime_RealtimeThread.cpp
        native/alcybe_realtime_RealtimeCamera.cpp)
    include(CheckIncludeFileCXX)
    check_include_file_cxx(numa.h hasNumaHeaders)
    find_library(numaLibrary numa)
    if(hasNumaHeaders AND numaLibrary)
        target_compile_definitions(realtime PRIVATE ALCYBE_WITH_NUMA=1)
        target_link_libraries(realtime PRIVATE "${numaLibrary}")
    endif()
    target_link_libraries(realtime PRIVATE pthread rt)
endif()

target_include_directories(realtime PRIVATE "${javaHeaders}" "${javaPlatformHeaders}")
if(WIN32)
    set_target_properties(realtime PROPERTIES PREFIX "")
endif()
if(MSVC)
    target_compile_options(realtime PRIVATE /W3 /O2)
else()
    target_compile_options(realtime PRIVATE -O3 -Wall -Wextra)
endif()
message(STATUS "Target: ${CMAKE_SYSTEM_NAME}; portable mock: ${ALCYBE_PORTABLE_TEST_BACKEND}")
