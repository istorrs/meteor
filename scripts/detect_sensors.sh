#!/bin/bash

# SSH into cameras, read the loaded sensor kernel module, and verify
# (or update) the sensor field in cameras.json.
#
# Usage:
#   detect_sensors.sh                Detect all cameras
#   detect_sensors.sh <name>         Detect a single camera by name
#   detect_sensors.sh --all <plat>   Detect all cameras of a platform
#
# The sensor module name is extracted from lsmod (e.g. sensor_gc2053_t31)
# and compared against field 4 of the board string:
#   wyze_cam3_t31x_gc2053_rtl8189ftv
#        f1    f2   f3   f4      f5
# If they differ, cameras.json is updated in place.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
INVENTORY="$SCRIPT_DIR/cameras.json"

# --- helper functions ---

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

detect_camera() {
    local name="$1"
    local ip="$2"
    local board="$3"

    printf "%-14s %-16s " "$name" "$ip"

    # SSH in and grab lsmod output
    local lsmod_output
    if ! lsmod_output="$(ssh -n -o ConnectTimeout=10 -o BatchMode=yes \
            "root@${ip}" lsmod 2>/dev/null)"; then
        echo "SKIP  (unreachable)"
        return
    fi

    # Find the sensor module line (e.g. "sensor_gc2053_t31 ...")
    local sensor_module
    sensor_module="$(echo "$lsmod_output" | awk '/^sensor_/{print $1}')"
    if [ -z "$sensor_module" ]; then
        echo "SKIP  (no sensor module)"
        return
    fi

    # Extract sensor name: sensor_<name>_<soc> → <name>
    local detected
    detected="$(echo "$sensor_module" | sed 's/^sensor_//; s/_[^_]*$//')"

    # Extract current sensor from board field 4
    local current
    current="$(echo "$board" | awk -F_ '{print $4}')"

    if [ "$detected" = "$current" ]; then
        echo "OK    (sensor: $detected)"
    else
        # Build new board string with detected sensor in field 4
        local new_board
        new_board="$(echo "$board" | awk -F_ -v s="$detected" \
            '{print $1"_"$2"_"$3"_"s"_"$5}')"

        jq --arg n "$name" --arg b "$new_board" \
            '(.[] | select(.name == $n)).board = $b' \
            "$INVENTORY" > "$INVENTORY.tmp" && mv "$INVENTORY.tmp" "$INVENTORY"

        echo "UPDATED  $board → $new_board"
    fi
}

# --- main ---

if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
    echo "Usage:"
    echo "  detect_sensors.sh                Detect all cameras"
    echo "  detect_sensors.sh <name>         Detect a single camera by name"
    echo "  detect_sensors.sh --all <plat>   Detect all cameras of a platform"
    exit 0
fi

if [ ! -f "$INVENTORY" ]; then
    echo "Error: camera inventory not found: $INVENTORY"
    exit 1
fi

if [ $# -eq 0 ]; then
    # No args: detect all cameras
    CAMERAS="$(jq -c '.[]' "$INVENTORY")"

    while IFS= read -r cam; do
        cam_name="$(echo "$cam" | jq -r '.name')"
        cam_ip="$(echo "$cam" | jq -r '.ip')"
        cam_board="$(echo "$cam" | jq -r '.board')"
        detect_camera "$cam_name" "$cam_ip" "$cam_board"
    done <<< "$CAMERAS"

elif [ "$1" = "--all" ]; then
    # --all <platform>
    if [ $# -lt 2 ]; then
        echo "Usage: detect_sensors.sh --all <platform>"
        exit 1
    fi
    FILTER_PLATFORM="$2"
    fp_lower="$(echo "$FILTER_PLATFORM" | tr '[:upper:]' '[:lower:]')"

    CAMERAS="$(jq -c --arg p "$fp_lower" \
        '.[] | select(.platform | ascii_downcase == $p)' "$INVENTORY")"

    while IFS= read -r cam; do
        cam_name="$(echo "$cam" | jq -r '.name')"
        cam_ip="$(echo "$cam" | jq -r '.ip')"
        cam_board="$(echo "$cam" | jq -r '.board')"
        detect_camera "$cam_name" "$cam_ip" "$cam_board"
    done <<< "$CAMERAS"

else
    # Single arg: camera name
    NAME="$1"
    ENTRY="$(lookup_camera "$NAME")"
    CAM_IP="$(echo "$ENTRY" | jq -r '.ip')"
    CAM_BOARD="$(echo "$ENTRY" | jq -r '.board')"
    detect_camera "$NAME" "$CAM_IP" "$CAM_BOARD"
fi
