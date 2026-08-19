#!/bin/sh
set -eu

DISPLAY_NUMBER=":96"
XVFB="/usr/bin/Xvfb"
BRIDGE="/usr/local/libexec/q2/q2-x11-fb-bridge"
CALIBRATOR="/usr/local/libexec/q2/q2-touch-calibrate"
LOG_DIR="/tmp/q2-touch-calibration"

xvfb_pid=""
bridge_pid=""

cleanup() {
    set +e
    [ -n "$bridge_pid" ] && kill "$bridge_pid" 2>/dev/null
    [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null
    wait 2>/dev/null
    systemctl restart makerbase-client.service
}

trap cleanup EXIT INT TERM HUP

rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"
rm -f "/tmp/.X96-lock"

systemctl stop makerbase-client.service

runuser -u qidi -- env HOME=/home/qidi "$XVFB" "$DISPLAY_NUMBER" \
    -screen 0 480x272x24 -nolisten tcp -noreset \
    >"$LOG_DIR/xvfb.log" 2>&1 &
xvfb_pid=$!

attempt=0
while [ ! -S /tmp/.X11-unix/X96 ]; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 50 ]; then
        printf 'Xvfb did not become ready.\n' >&2
        exit 1
    fi
    sleep 0.1
done

runuser -u qidi -- env HOME=/home/qidi DISPLAY="$DISPLAY_NUMBER" \
    Q2_BRIDGE_FPS=15 "$BRIDGE" \
    >"$LOG_DIR/bridge.log" 2>&1 &
bridge_pid=$!

runuser -u qidi -- env HOME=/home/qidi DISPLAY="$DISPLAY_NUMBER" \
    "$CALIBRATOR" | tee "$LOG_DIR/points.log"
