#!/usr/bin/env bash
set -uo pipefail

SERVE_DIR="${SERVE_DIR:-/data/code/Baklava/yolov8s-obb-cpp/data/.serve}"
BIN="${BIN:-/app/sar_ship_detect}"
LOG_MAX_BYTES="${LOG_MAX_BYTES:-16777216}"
BACKOFF_MIN="${BACKOFF_MIN:-1}"
BACKOFF_MAX="${BACKOFF_MAX:-30}"
CRASH_WINDOW="${CRASH_WINDOW:-60}"
CRASH_BURST="${CRASH_BURST:-5}"

LOG="$SERVE_DIR/serve.log"
STATE="$SERVE_DIR/state"
STOP="$SERVE_DIR/stop"

mkdir -p "$SERVE_DIR"
chmod 0777 "$SERVE_DIR" 2>/dev/null || true

restarts=0
serve_pid=""
supervisor_started=$(date +%s)

write_state() {
    local phase=$1
    {
        echo "supervisor_pid=$$"
        echo "supervisor_started=$supervisor_started"
        echo "serve_pid=${serve_pid:-}"
        echo "serve_started=${serve_started:-}"
        echo "restarts=$restarts"
        echo "phase=$phase"
        echo "updated=$(date +%s)"
    } > "$STATE.tmp" && mv "$STATE.tmp" "$STATE"
    chmod 0666 "$STATE" 2>/dev/null || true
}

shutdown() {
    write_state stopping
    if [[ -n "$serve_pid" ]] && kill -0 "$serve_pid" 2>/dev/null; then
        kill -TERM "$serve_pid" 2>/dev/null
        for _ in $(seq 1 50); do
            kill -0 "$serve_pid" 2>/dev/null || break
            sleep 0.1
        done
        kill -KILL "$serve_pid" 2>/dev/null
    fi
    write_state down
    exit 0
}
trap shutdown TERM INT

rotate_log() {
    local sz
    sz=$(stat -c %s "$LOG" 2>/dev/null || echo 0)
    if (( sz > LOG_MAX_BYTES )); then
        mv -f "$LOG" "$LOG.1" 2>/dev/null || true
    fi
}

echo "[supervisor] starting, serve dir $SERVE_DIR" >> "$LOG"
burst=0
burst_since=$(date +%s)

while true; do
    if [[ -e "$STOP" ]]; then
        serve_pid=""
        write_state stopped
        sleep 2
        continue
    fi

    rotate_log
    serve_started=$(date +%s)
    echo "[supervisor] launching $BIN --serve (restart #$restarts)" >> "$LOG"
    "$BIN" --serve "$SERVE_DIR" >> "$LOG" 2>&1 &
    serve_pid=$!
    write_state running

    wait "$serve_pid"
    rc=$?
    serve_pid=""

    if [[ -e "$STOP" ]]; then
        echo "[supervisor] serve exited rc=$rc and a stop flag is set; idling" >> "$LOG"
        continue
    fi

    restarts=$((restarts + 1))
    echo "[supervisor] serve exited rc=$rc after $(( $(date +%s) - serve_started ))s" >> "$LOG"

    now=$(date +%s)
    if (( now - burst_since > CRASH_WINDOW )); then
        burst=0
        burst_since=$now
    fi
    burst=$((burst + 1))

    delay=$BACKOFF_MIN
    if (( burst > CRASH_BURST )); then
        delay=$BACKOFF_MAX
        echo "[supervisor] $burst exits within ${CRASH_WINDOW}s; backing off ${delay}s" >> "$LOG"
    fi
    write_state restarting
    sleep "$delay"
done
