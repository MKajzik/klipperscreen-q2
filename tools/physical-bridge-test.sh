#!/bin/sh
set -eu

DISPLAY_NUMBER=":98"
BRIDGE="/usr/local/libexec/q2/q2-x11-fb-bridge"
XVFB="/usr/bin/Xvfb"
LOG_DIR="/tmp/q2-physical-test"

xvfb_pid=""
message_pid=""
bridge_pid=""

cleanup() {
    set +e
    [ -n "$bridge_pid" ] && kill "$bridge_pid" 2>/dev/null
    [ -n "$message_pid" ] && kill "$message_pid" 2>/dev/null
    [ -n "$xvfb_pid" ] && kill "$xvfb_pid" 2>/dev/null
    wait 2>/dev/null
    systemctl restart makerbase-client.service
}

trap cleanup EXIT INT TERM HUP

rm -rf "$LOG_DIR"
mkdir -p "$LOG_DIR"
rm -f "/tmp/.X98-lock"

systemctl stop makerbase-client.service

runuser -u qidi -- env HOME=/home/qidi "$XVFB" "$DISPLAY_NUMBER" \
    -screen 0 480x272x24 -nolisten tcp -noreset \
    >"$LOG_DIR/xvfb.log" 2>&1 &
xvfb_pid=$!

attempt=0
while [ ! -S /tmp/.X11-unix/X98 ]; do
    attempt=$((attempt + 1))
    if [ "$attempt" -ge 50 ]; then
        printf 'Xvfb did not become ready.\n' >&2
        exit 1
    fi
    sleep 0.1
done

runuser -u qidi -- env HOME=/home/qidi DISPLAY="$DISPLAY_NUMBER" \
    xmessage -buttons "TOUCH OK:0" -default "TOUCH OK" \
    -geometry 440x210+20+31 \
    "QIDI Q2 DISPLAY BRIDGE TEST

If you see this on the printer,
tap the TOUCH OK button." \
    >"$LOG_DIR/xmessage.log" 2>&1 &
message_pid=$!

runuser -u qidi -- env HOME=/home/qidi DISPLAY="$DISPLAY_NUMBER" \
    Q2_BRIDGE_FPS=20 "$BRIDGE" \
    >"$LOG_DIR/bridge.log" 2>&1 &
bridge_pid=$!

seconds=0
while [ "$seconds" -lt 25 ]; do
    if ! kill -0 "$message_pid" 2>/dev/null; then
        printf 'TOUCH_CONFIRMED\n'
        exit 0
    fi
    if ! kill -0 "$bridge_pid" 2>/dev/null; then
        printf 'Bridge stopped unexpectedly.\n' >&2
        exit 1
    fi
    sleep 1
    seconds=$((seconds + 1))
done

printf 'DISPLAY_CONFIRMED_TOUCH_NOT_OBSERVED\n'
