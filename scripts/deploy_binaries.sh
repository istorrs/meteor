#!/bin/bash

# Quick rebuild of meteor binary (thingino/MIPS) and SCP to camera /tmp.
# Does NOT rebuild firmware — just pushes binary for immediate testing.
#
# Usage:
#   deploy_binaries.sh <name>           Deploy to a camera by name (from cameras.json)
#   deploy_binaries.sh <platform> <ip>  Deploy to a specific IP (original behavior)
#   deploy_binaries.sh --all            Deploy to all cameras in cameras.json
#   deploy_binaries.sh --all <platform> Deploy to all cameras of a given platform
#
# On the camera, run from /tmp:
#   /tmp/meteor

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$SCRIPT_DIR/.."
INVENTORY="$SCRIPT_DIR/cameras.json"
THINGINO_DIR="${THINGINO_DIR:-$HOME/work/github/thingino-firmware}"
export THINGINO_DIR

# --- helper functions ---

build_platform() {
    local platform="$1"
    local platform_upper
    platform_upper="$(echo "$platform" | tr '[:lower:]' '[:upper:]')"
    local build_dir="$PROJECT_DIR/build-${platform}"
    local toolchain="$PROJECT_DIR/cmake/toolchain-${platform}.cmake"

    if [ ! -f "$toolchain" ]; then
        echo "Error: toolchain file not found: $toolchain"
        echo "Valid platforms: t31, t20"
        exit 1
    fi

    echo "=== Building meteor for ${platform_upper} ==="
    mkdir -p "$build_dir"
    pushd "$build_dir" > /dev/null
    cmake -DCMAKE_TOOLCHAIN_FILE="$toolchain" ..
    make
    popd > /dev/null
}

deploy_to_camera() {
    local platform="$1"
    local ip="$2"
    local name="${3:-}"
    local build_dir="$PROJECT_DIR/build-${platform}"
    local label="$ip"
    [ -n "$name" ] && label="$name ($ip)"

    echo ""
    echo "=== Pushing binaries to root@${label}:/tmp/ ==="
    scp -O "$build_dir/meteor" "$build_dir/astrostack" \
        "$PROJECT_DIR/scripts/nightsky.sh" "root@${ip}:/tmp/"
    echo "  Done: $label"
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
    echo "  deploy_binaries.sh <name>           Deploy to a camera by name"
    echo "  deploy_binaries.sh <platform> <ip>  Deploy to a specific IP"
    echo "  deploy_binaries.sh --all            Deploy to all cameras"
    echo "  deploy_binaries.sh --all <platform> Deploy to all cameras of a platform"
    exit 1
fi

if [ "$1" = "--all" ]; then
    # --all [platform]
    FILTER_PLATFORM="${2:-}"

    if [ ! -f "$INVENTORY" ]; then
        echo "Error: camera inventory not found: $INVENTORY"
        exit 1
    fi

    # Collect the unique platforms we need to build (normalize to lowercase)
    if [ -n "$FILTER_PLATFORM" ]; then
        PLATFORMS="$(echo "$FILTER_PLATFORM" | tr '[:upper:]' '[:lower:]')"
    else
        PLATFORMS="$(jq -r '.[].platform' "$INVENTORY" | tr '[:upper:]' '[:lower:]' | sort -u)"
    fi

    # Build once per platform, then deploy to all cameras of that platform
    for plat in $PLATFORMS; do
        build_platform "$plat"

        CAMERAS="$(jq -c --arg p "$plat" '.[] | select(.platform | ascii_downcase == $p)' "$INVENTORY")"

        while IFS= read -r cam; do
            cam_ip="$(echo "$cam" | jq -r '.ip')"
            cam_name="$(echo "$cam" | jq -r '.name')"
            deploy_to_camera "$plat" "$cam_ip" "$cam_name"
        done <<< "$CAMERAS"
    done

    echo ""
    echo "=== All deployments complete ==="

elif [ $# -eq 2 ]; then
    # Two args: platform + ip (original behavior)
    PLATFORM="$(echo "$1" | tr '[:upper:]' '[:lower:]')"
    CAMERA_IP="$2"
    build_platform "$PLATFORM"
    deploy_to_camera "$PLATFORM" "$CAMERA_IP"

    echo ""
    echo "=== Done. Test on camera with: ==="
    echo "  ssh root@${CAMERA_IP}"
    echo "  /tmp/meteor"

else
    # Single arg: camera name lookup
    NAME="$1"
    ENTRY="$(lookup_camera "$NAME")"
    PLATFORM="$(echo "$ENTRY" | jq -r '.platform | ascii_downcase')"
    CAMERA_IP="$(echo "$ENTRY" | jq -r '.ip')"

    build_platform "$PLATFORM"
    deploy_to_camera "$PLATFORM" "$CAMERA_IP" "$NAME"

    echo ""
    echo "=== Done. Test on camera with: ==="
    echo "  ssh root@${CAMERA_IP}"
    echo "  /tmp/meteor"
fi
