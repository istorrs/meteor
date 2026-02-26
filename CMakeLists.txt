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

# Source files
set(SOURCES
    src/main.c
    src/system.c
    src/isp.c
    src/isp_tuning.c
    src/framesource.c
    src/ivs.c
    src/log.c
    src/jpeg.c
    src/config.c
    src/event.c
    src/capture.c
)

# Build meteor executable
add_executable(meteor ${SOURCES})

# Link libraries
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
