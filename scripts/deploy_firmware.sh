#!/bin/bash

# Rebuild meteor binary (thingino/MIPS), rebuild firmware image, and
# push the update to a camera running thingino.
#
# Usage:
#   deploy_firmware.sh <name>           Deploy to a camera by name (from cameras.json)
#   deploy_firmware.sh <board> <ip>     Deploy to a specific IP with given board
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
THINGINO_DIR="${THINGINO_DIR:-$HOME/work/github/thingino-firmware}"
export THINGINO_DIR
OVERLAY_BIN="$THINGINO_DIR/overlay/lower/usr/bin"

# --- helper functions ---

board_to_platform() {
    local board="$1"
    local soc
    soc="$(echo "$board" | cut -d'_' -f3)"
    # Strip trailing letter (t31x → t31, t20x → t20)
    echo "${soc%?}"
}

build_firmware() {
    local board="$1"
    local platform
    platform="$(board_to_platform "$board")"
    local platform_upper
    platform_upper="$(echo "$platform" | tr '[:lower:]' '[:upper:]')"
    local build_dir="$PROJECT_DIR/build-${platform}"
    local toolchain="$PROJECT_DIR/cmake/toolchain-${platform}.cmake"

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
    echo "=== Step 2: Copy binaries to thingino overlay ==="
    cp "$build_dir/meteor" "$OVERLAY_BIN/meteor"
    cp "$build_dir/astrostack" "$OVERLAY_BIN/astrostack"
    cp "$PROJECT_DIR/scripts/nightsky.sh" "$OVERLAY_BIN/nightsky"
    ls -la "$OVERLAY_BIN"/meteor "$OVERLAY_BIN"/astrostack "$OVERLAY_BIN"/nightsky

    echo ""
    echo "=== Step 3: Rebuild thingino firmware image ==="
    cd "$THINGINO_DIR"
    make BOARD="$board" all

    local output_dir="$HOME/output/stable/${board}-3.10"
    local update_image="$output_dir/images/thingino-${board}-update.bin"
    echo "Firmware image: $update_image"
}

flash_camera() {
    local board="$1"
    local ip="$2"
    local name="${3:-}"
    local output_dir="$HOME/output/stable/${board}-3.10"
    local update_image="$output_dir/images/thingino-${board}-update.bin"
    local label="$ip"
    [ -n "$name" ] && label="$name ($ip)"

    echo ""
    echo "=== Pushing firmware to ${label} ==="
    scp -O "$update_image" "root@${ip}:/tmp/"
    echo "Starting sysupgrade on ${label}..."
    # Run sysupgrade fully detached so SSH returns immediately.
    # The camera will flash and reboot on its own.
    ssh -o ConnectTimeout=10 "root@${ip}" \
        "nohup sysupgrade /tmp/thingino-${board}-update.bin </dev/null >/dev/null 2>&1 &"
    echo "  Done: $label — camera is flashing and will reboot"
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

if [ $# -eq 0 ] || [ "$1" = "--help" ] || [ "$1" = "-h" ]; then
    echo "Usage:"
    echo "  deploy_firmware.sh <name>           Deploy to a camera by name"
    echo "  deploy_firmware.sh <board> <ip>     Deploy to a specific IP with given board"
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

    # Collect unique boards to build
    if [ -n "$FILTER_PLATFORM" ]; then
        fp_lower="$(echo "$FILTER_PLATFORM" | tr '[:upper:]' '[:lower:]')"
        BOARDS="$(jq -r --arg p "$fp_lower" \
            '[.[] | select(.platform | ascii_downcase == $p) | .board] | unique | .[]' \
            "$INVENTORY")"
    else
        BOARDS="$(jq -r '[.[].board] | unique | .[]' "$INVENTORY")"
    fi

    # Build once per board, then flash all cameras with that board
    for board in $BOARDS; do
        build_firmware "$board"

        CAMERAS="$(jq -c --arg b "$board" '.[] | select(.board == $b)' "$INVENTORY")"

        while IFS= read -r cam; do
            cam_ip="$(echo "$cam" | jq -r '.ip')"
            cam_name="$(echo "$cam" | jq -r '.name')"
            flash_camera "$board" "$cam_ip" "$cam_name"
        done <<< "$CAMERAS"
    done

    echo ""
    echo "=== All firmware deployments complete ==="

elif [ $# -eq 2 ]; then
    # Two args: board + ip
    BOARD="$1"
    CAMERA_IP="$2"
    build_firmware "$BOARD"
    flash_camera "$BOARD" "$CAMERA_IP"

    echo ""
    echo "=== Done. Camera will reboot with updated firmware. ==="

else
    # Single arg: camera name lookup
    NAME="$1"
    ENTRY="$(lookup_camera "$NAME")"
    BOARD="$(echo "$ENTRY" | jq -r '.board')"
    CAMERA_IP="$(echo "$ENTRY" | jq -r '.ip')"

    build_firmware "$BOARD"
    flash_camera "$BOARD" "$CAMERA_IP" "$NAME"

    echo ""
    echo "=== Done. Camera will reboot with updated firmware. ==="
fi
