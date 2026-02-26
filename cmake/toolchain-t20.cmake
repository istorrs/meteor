# Cross-compilation toolchain file for Ingenic T20X (MIPS32 Little-Endian)
# Usage:
#   cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-t20.cmake ..
#
# Override THINGINO_OUTPUT to point to a different build output directory:
#   cmake -DCMAKE_TOOLCHAIN_FILE=../cmake/toolchain-t20.cmake \
#         -DTHINGINO_OUTPUT=$HOME/output/stable/wyze_cam2_t20x_jxf22_rtl8189ftv-3.10 ..

cmake_minimum_required(VERSION 3.10)

set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR mipsel)

# Thingino build output directory
if(NOT DEFINED THINGINO_OUTPUT)
    if(DEFINED ENV{THINGINO_OUTPUT})
        set(THINGINO_OUTPUT "$ENV{THINGINO_OUTPUT}" CACHE PATH "Thingino build output directory")
    else()
        set(THINGINO_OUTPUT "$ENV{HOME}/output/stable/wyze_cam2_t20x_jxf22_rtl8189ftv-3.10"
            CACHE PATH "Thingino build output directory")
    endif()
endif()

if(NOT EXISTS "${THINGINO_OUTPUT}/host/bin")
    message(FATAL_ERROR
        "THINGINO_OUTPUT does not point to a valid build output.\n"
        "Expected: ${THINGINO_OUTPUT}/host/bin\n"
        "Pass -DTHINGINO_OUTPUT=/path/to/output on the cmake command line.")
endif()

# Optional: thingino-firmware source tree (for SDK headers in dl/)
if(NOT DEFINED THINGINO_DIR OR THINGINO_DIR STREQUAL "")
    if(DEFINED ENV{THINGINO_DIR})
        set(THINGINO_DIR "$ENV{THINGINO_DIR}" CACHE PATH "Path to thingino-firmware source" FORCE)
    else()
        set(THINGINO_DIR "" CACHE PATH "Path to thingino-firmware source (optional, for dl/ fallback)")
    endif()
endif()

# Toolchain
set(CROSS_COMPILE "mipsel-linux-" CACHE STRING "Cross-compilation prefix")
set(CMAKE_C_COMPILER   "${THINGINO_OUTPUT}/host/bin/${CROSS_COMPILE}gcc")
set(CMAKE_CXX_COMPILER "${THINGINO_OUTPUT}/host/bin/${CROSS_COMPILE}g++")

# Sysroot — staging directory has headers and libraries
set(CMAKE_SYSROOT "${THINGINO_OUTPUT}/staging")
set(CMAKE_FIND_ROOT_PATH "${CMAKE_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)

# Platform identifier — used by CMakeLists.txt to set -DPLATFORM_T20
set(METEOR_PLATFORM "T20" CACHE STRING "Target platform (T31 or T20)")

# SDK paths — IMP headers from prudynt build directory
set(SOC_FAMILY  "T20"    CACHE STRING "SoC family")
set(SDK_VERSION "3.12.0" CACHE STRING "SDK version")

# Find prudynt include dir in build output (hash in dirname varies)
# The zh/ directory contains imp/ and sysutils/ subdirectories,
# so we include the parent so that #include <imp/...> and <sysutils/...> work.
# T20 SDK headers also use bare #include <imp_common.h> internally,
# so the imp/ subdirectory itself must be on the include path too.
file(GLOB _PRUDYNT_DIRS "${THINGINO_OUTPUT}/build/prudynt-t-*/include/${SOC_FAMILY}/${SDK_VERSION}/zh")
if(_PRUDYNT_DIRS)
    list(GET _PRUDYNT_DIRS 0 _PRUDYNT_INC)
    set(IMP_SDK_INC_DIR "${_PRUDYNT_INC};${_PRUDYNT_INC}/imp" CACHE PATH "IMP/sysutils SDK header directory")
elseif(THINGINO_DIR AND EXISTS "${THINGINO_DIR}/dl/prudynt-t/git/include/${SOC_FAMILY}/${SDK_VERSION}/zh/imp")
    set(_PRUDYNT_INC "${THINGINO_DIR}/dl/prudynt-t/git/include/${SOC_FAMILY}/${SDK_VERSION}/zh")
    set(IMP_SDK_INC_DIR "${_PRUDYNT_INC};${_PRUDYNT_INC}/imp"
        CACHE PATH "IMP/sysutils SDK header directory")
else()
    message(FATAL_ERROR "Cannot find IMP SDK headers in build output or THINGINO_DIR.")
endif()

# IMP libraries are installed in staging sysroot
set(IMP_SDK_LIB_DIR "${THINGINO_OUTPUT}/staging/usr/lib" CACHE PATH "IMP SDK library directory")
