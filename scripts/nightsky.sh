#!/bin/sh
#
# nightsky — hardware setup wrapper for night sky applications
#
# Prepares the camera hardware for astrophotography or meteor detection:
#   1. Stops majestic (releases ISP for exclusive use)
#   2. Stops the day/night daemon (prevents IR/filter fighting)
#   3. Turns off IR illumination LEDs (no glare)
#   4. Removes the IR cut filter (full spectrum sensitivity)
#   5. Turns off status LEDs (no light leaks)
#   6. Runs the specified application
#   7. Restores previous state on exit (restarts majestic + daemon)
#
# Usage:
#   nightsky.sh <program> [args...]
#
# Examples:
#   nightsky.sh /tmp/meteor
#   nightsky.sh /tmp/astrostack -n 30 -e 5 -c -o /tmp/stack.ppm

set -u

PROG_NAME="$(basename "$0")"

log() {
	echo "[$PROG_NAME] $*"
}

err() {
	echo "[$PROG_NAME] ERROR: $*" >&2
}

if [ $# -lt 1 ]; then
	echo "Usage: $PROG_NAME <program> [args...]" >&2
	echo "  Prepares camera hardware for night sky use, then runs <program>." >&2
	exit 1
fi

# --- save previous state ---

MAJESTIC_WAS_RUNNING=0
if pidof majestic >/dev/null 2>&1; then
	MAJESTIC_WAS_RUNNING=1
fi

DAYNIGHTD_WAS_RUNNING=0
if pidof daynightd >/dev/null 2>&1; then
	DAYNIGHTD_WAS_RUNNING=1
fi

IRCUT_WAS_ON=0
if [ -f /tmp/ircutmode.txt ]; then
	case "$(cat /tmp/ircutmode.txt)" in
		on) IRCUT_WAS_ON=1 ;;
	esac
fi

# --- restore on exit ---

cleanup() {
	log "restoring hardware state..."

	# Restore IR cut filter
	if [ "$IRCUT_WAS_ON" -eq 1 ]; then
		log "  IR cut filter -> on"
		ircut on >/dev/null 2>&1
	fi

	# Restore IR LEDs (daemon will manage them if restarted)

	# Restart day/night daemon if it was running
	if [ "$DAYNIGHTD_WAS_RUNNING" -eq 1 ]; then
		log "  restarting daynightd"
		/etc/init.d/S*daynightd start >/dev/null 2>&1 || \
			daynightd >/dev/null 2>&1 || true
	fi

	# Restart majestic if it was running
	if [ "$MAJESTIC_WAS_RUNNING" -eq 1 ]; then
		log "  restarting majestic"
		/etc/init.d/S95majestic start >/dev/null 2>&1 || \
			majestic >/dev/null 2>&1 || true
	fi

	log "done"
}

trap cleanup EXIT INT TERM

# --- hardware setup ---

# 1. Stop majestic (must release ISP before our app can use it)
if [ "$MAJESTIC_WAS_RUNNING" -eq 1 ]; then
	log "stopping majestic..."
	/etc/init.d/S95majestic stop >/dev/null 2>&1 || \
		killall majestic >/dev/null 2>&1 || true
	sleep 2
fi

# 2. Stop day/night daemon
if [ "$DAYNIGHTD_WAS_RUNNING" -eq 1 ]; then
	log "stopping daynightd..."
	/etc/init.d/S*daynightd stop >/dev/null 2>&1 || \
		killall daynightd >/dev/null 2>&1 || true
	sleep 1
fi

# 3. Turn off IR LEDs
log "turning off IR LEDs..."
irled off >/dev/null 2>&1 || log "  irled not available (ok)"

# 4. Remove IR cut filter (allow full spectrum)
log "removing IR cut filter..."
ircut off >/dev/null 2>&1 || log "  ircut not available (ok)"

# 5. Turn off status LEDs
log "turning off status LEDs..."
for led in /sys/class/leds/*/brightness; do
	[ -f "$led" ] && echo 0 > "$led" 2>/dev/null
done

log "hardware ready for night sky"

# --- run the application ---

log "starting: $*"
"$@"
RET=$?

if [ $RET -ne 0 ]; then
	err "program exited with code $RET"
fi

exit $RET
