#!/bin/bash

# Rebuild meteor binary (thingino/MIPS), rebuild firmware image, and
# push the update to a camera running thingino.
#
# Usage:
#   deploy_firmware.sh <name>           Deploy to a camera by name (from cameras.json)
#   deploy_firmware.sh <platform> <ip>  Deploy to a specific IP (original behavior)
#   deploy_firmware.sh --all            Deploy to all cameras in cameras.json
#   deploy_firmware.sh --all <platform> Deploy to all cameras of a given platform
#
# Prerequisites:
#   - Thingino build tree at THINGINO_DIR (already built once)
#   - Thingino cross-compiler available in the build tree
#   - Camera accessible via SSH (root@camera_ip)

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/.."
INVENTORY="$SCRIPT_DIR/cameras.json"
THINGINO_DIR="$HOME/work/github/thingino-firmware"
OVERLAY_BIN="$THINGINO_DIR/overlay/lower/usr/bin"

# --- helper functions ---

platform_to_board() {
    local platform="$1"
    case "$platform" in
        t31) echo "wyze_cam3_t31x_gc2053_rtl8189ftv" ;;
        t20) echo "wyze_cam2_t20x_jxf22_rtl8189ftv" ;;
        *)
            echo "Error: unknown platform '$platform'"
            echo "Valid platforms: t31, t20"
            exit 1
            ;;
    esac
}

build_firmware() {
    local platform="$1"
    local platform_upper
    platform_upper="$(echo "$platform" | tr '[:lower:]' '[:upper:]')"
    local build_dir="$PROJECT_DIR/build-${platform}"
    local toolchain="$PROJECT_DIR/cmake/toolchain-${platform}.cmake"
    local board
    board="$(platform_to_board "$platform")"

    if [ ! -f "$toolchain" ]; then
        echo "Error: toolchain file not found: $toolchain"
        exit 1
    fi

    echo "=== Step 1: Build meteor binary for ${platform_upper} ==="
    mkdir -p "$build_dir"
    pushd "$build_dir" > /dev/null
    cmake -DCMAKE_TOOLCHAIN_FILE="$toolchain" ..
    make
    popd > /dev/null

    echo ""
    echo "=== Step 2: Copy binary to thingino overlay ==="
    cp "$build_dir/meteor" "$OVERLAY_BIN/meteor"
    ls -la "$OVERLAY_BIN"/meteor

    echo ""
    echo "=== Step 3: Rebuild thingino firmware image ==="
    cd "$THINGINO_DIR"
    make BOARD="$board" all

    local output_dir="$HOME/output/stable/${board}-3.10"
    local update_image="$output_dir/images/thingino-${board}-update.bin"
    echo "Firmware image: $update_image"
}

flash_camera() {
    local platform="$1"
    local ip="$2"
    local name="${3:-}"
    local board
    board="$(platform_to_board "$platform")"
    local output_dir="$HOME/output/stable/${board}-3.10"
    local update_image="$output_dir/images/thingino-${board}-update.bin"
    local label="$ip"
    [ -n "$name" ] && label="$name ($ip)"

    echo ""
    echo "=== Pushing firmware to ${label} ==="
    scp -O "$update_image" "root@${ip}:/tmp/"
    echo "Starting sysupgrade on ${label}..."
    ssh "root@${ip}" "sysupgrade /tmp/thingino-${board}-update.bin"
    echo "  Done: $label — camera will reboot with updated firmware"
}

lookup_camera() {
    local name="$1"
    if [ ! -f "$INVENTORY" ]; then
        echo "Error: camera inventory not found: $INVENTORY"
        exit 1
    fi
    local entry
    entry="$(jq -e --arg n "$name" '.[] | select(.name == $n)' "$INVENTORY" 2>/dev/null)" || {
        echo "Error: camera '$name' not found in $INVENTORY"
        echo "Available cameras:"
        jq -r '.[] | "  \(.name)  (\(.platform), \(.ip))"' "$INVENTORY"
        exit 1
    }
    echo "$entry"
}

# --- main ---

if [ $# -eq 0 ]; then
    echo "Usage:"
    echo "  deploy_firmware.sh <name>           Deploy to a camera by name"
    echo "  deploy_firmware.sh <platform> <ip>  Deploy to a specific IP"
    echo "  deploy_firmware.sh --all            Deploy to all cameras"
    echo "  deploy_firmware.sh --all <platform> Deploy to all cameras of a platform"
    exit 1
fi

if [ "$1" = "--all" ]; then
    # --all [platform]
    FILTER_PLATFORM="${2:-}"

    if [ ! -f "$INVENTORY" ]; then
        echo "Error: camera inventory not found: $INVENTORY"
        exit 1
    fi

    # Collect the unique platforms we need to build
    if [ -n "$FILTER_PLATFORM" ]; then
        PLATFORMS="$FILTER_PLATFORM"
    else
        PLATFORMS="$(jq -r '.[].platform' "$INVENTORY" | sort -u)"
    fi

    # Build firmware once per platform, then flash all cameras of that platform
    for plat in $PLATFORMS; do
        build_firmware "$plat"

        CAMERAS="$(jq -c --arg p "$plat" '.[] | select(.platform == $p)' "$INVENTORY")"

        while IFS= read -r cam; do
            cam_ip="$(echo "$cam" | jq -r '.ip')"
            cam_name="$(echo "$cam" | jq -r '.name')"
            flash_camera "$plat" "$cam_ip" "$cam_name"
        done <<< "$CAMERAS"
    done

    echo ""
    echo "=== All firmware deployments complete ==="

elif [ $# -eq 2 ]; then
    # Two args: platform + ip (original behavior)
    PLATFORM="$1"
    CAMERA_IP="$2"
    build_firmware "$PLATFORM"
    flash_camera "$PLATFORM" "$CAMERA_IP"

    echo ""
    echo "=== Done. Camera will reboot with updated firmware. ==="

else
    # Single arg: camera name lookup
    NAME="$1"
    ENTRY="$(lookup_camera "$NAME")"
    PLATFORM="$(echo "$ENTRY" | jq -r '.platform')"
    CAMERA_IP="$(echo "$ENTRY" | jq -r '.ip')"

    build_firmware "$PLATFORM"
    flash_camera "$PLATFORM" "$CAMERA_IP" "$NAME"

    echo ""
    echo "=== Done. Camera will reboot with updated firmware. ==="
fi
