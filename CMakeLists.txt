# meteor — ISP + IVS motion detection for Ingenic T31X / T20X
#
# Build (Wyze Cam V3 — T31):
#   mkdir -p build-t31 && cd build-t31
#   cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-t31.cmake ..
#   make
#
# Build (Wyze Cam V2 — T20):
#   mkdir -p build-t20 && cd build-t20
#   cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-t20.cmake ..
#   make

cmake_minimum_required(VERSION 3.10)
project(meteor C)

# Export compile_commands.json for clangd
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Platform define — set by toolchain file (defaults to T31)
if(NOT DEFINED METEOR_PLATFORM)
    set(METEOR_PLATFORM "T31")
endif()
add_definitions(-DPLATFORM_${METEOR_PLATFORM})
message(STATUS "Building for platform: ${METEOR_PLATFORM}")

# Include directories — project headers + SDK headers (imp/ and sysutils/ under IMP_SDK_INC_DIR)
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${IMP_SDK_INC_DIR}
)

# Library search path
link_directories(${IMP_SDK_LIB_DIR})

# Compiler flags
set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} -Wall -Wextra -O2 -march=mips32r2")

# Common IMP pipeline sources (shared by both executables)
set(COMMON_SOURCES
    src/system.c
    src/isp.c
    src/isp_tuning.c
    src/framesource.c
    src/log.c
    src/config.c
)

# meteor — IVS motion detection + JPEG capture
set(METEOR_SOURCES
    ${COMMON_SOURCES}
    src/main.c
    src/ivs.c
    src/jpeg.c
    src/event.c
    src/capture.c
)

add_executable(meteor ${METEOR_SOURCES})

# muslshim: bridges uclibc symbols that libimp.so expects to musl equivalents
target_link_libraries(meteor
    imp
    alog
    sysutils
    muslshim
    jpeg
    pthread
    m
    rt
)

# detector — RMS FTP meteor detector (no IVS, no JPEG capture)
set(DETECTOR_SOURCES
    ${COMMON_SOURCES}
    src/detector_main.c
    src/ftp.c
    src/hough.c
    src/ff_writer.c
    src/event_push.c
    src/detector.c
    src/meteor_module.c
)

add_executable(detector ${DETECTOR_SOURCES})

target_link_libraries(detector
    imp
    alog
    sysutils
    muslshim
    pthread
    m
    rt
)

# nightcam — combined FTP meteor detector + timelapse stacker + IVS metadata
set(NIGHTCAM_SOURCES
    ${COMMON_SOURCES}
    src/nightcam_main.c
    src/ftp.c
    src/hough.c
    src/ff_writer.c
    src/event_push.c
    src/detector.c
    src/ivs.c
    src/ivs_monitor.c
    src/stacker.c
)

add_executable(nightcam ${NIGHTCAM_SOURCES})

target_link_libraries(nightcam
    imp
    alog
    sysutils
    muslshim
    jpeg
    pthread
    m
    rt
)

# Build astrostack executable (standalone astrophotography stacker)
add_executable(astrostack src/astrostack.c src/jpeg.c src/log.c)

target_link_libraries(astrostack
    imp
    alog
    sysutils
    muslshim
    jpeg
    pthread
    m
    rt
)
