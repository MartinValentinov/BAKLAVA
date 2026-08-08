#!/usr/bin/env bash
set -euo pipefail

# Fixed container-internal layout -- a single /data bind mount from the host's
# yolov8s-obb-cpp/data/ (self-contained: everything the container needs lives
# under one host folder, mirrored 1:1 into the container):
#   /data/input          -- scenes land here (bind-mounted from the host)
#   /data/output         -- <scene_stem>.json (+ .jpg if DEBUG_JPG=1) land here
#   /data/model          -- model_b16_640.onnx, and the cached .engine once built
#   /data/GSHHS_shp/f    -- GSHHS_f_L1..L6.shp (already the on-disk layout)
# All overridable via env vars for local testing without touching the image.
INPUT_DIR="${INPUT_DIR:-/data/input}"
OUTPUT_DIR="${OUTPUT_DIR:-/data/output}"
MODEL_DIR="${MODEL_DIR:-/data/model}"
GSHHG_DIR="${GSHHG_DIR:-/data/GSHHS_shp/f}"
ONNX="${ONNX:-$MODEL_DIR/model_b16_640.onnx}"
ENGINE="${ENGINE:-$MODEL_DIR/model_b16_640.engine}"
POLL_SECONDS="${POLL_SECONDS:-10}"
DEBUG_JPG="${DEBUG_JPG:-0}"
BIN="${BIN:-/app/sar_ship_detect}"

GSHHG_ARGS=()
for lvl in L1 L2 L3 L4 L5 L6; do
    GSHHG_ARGS+=(--gshhg "$GSHHG_DIR/GSHHS_f_${lvl}.shp")
done

mkdir -p "$OUTPUT_DIR"

# Engine build is a one-time, GPU/TensorRT-version-locked cost (~10+ min on
# first ever start). Do it once up front, synchronously, so the first scene
# in the input directory isn't the one that eats it under a processing lock.
if [[ ! -f "$ENGINE" ]]; then
    echo "[service] no cached engine at $ENGINE -- building before watching $INPUT_DIR"
fi

process_one() {
    local tif="$1"
    local stem
    stem="$(basename "${tif%.tif}")"
    local out_json="$OUTPUT_DIR/${stem}.json"
    local marker="$OUTPUT_DIR/.${stem}.processing"

    [[ -f "$out_json" ]] && return 0   # already done, idempotent restart
    [[ -f "$marker" ]] && return 0     # a previous crashed attempt; needs manual look, don't loop on it
    touch "$marker"

    local jpg_args=()
    [[ "$DEBUG_JPG" == "1" ]] && jpg_args=(--out-jpg "$OUTPUT_DIR/${stem}.jpg")

    echo "[service] processing $tif"
    if "$BIN" \
        --tif "$tif" \
        --onnx "$ONNX" --engine "$ENGINE" \
        "${GSHHG_ARGS[@]}" \
        --out-json "${out_json}.tmp" \
        "${jpg_args[@]}" \
        -v; then
        mv "${out_json}.tmp" "$out_json"
        rm -f "$marker"
        echo "[service] done $tif -> $out_json"
    else
        echo "[service] FAILED $tif -- leaving $marker so it isn't silently retried" >&2
        rm -f "${out_json}.tmp"
    fi
}

echo "[service] watching $INPUT_DIR every ${POLL_SECONDS}s"
shopt -s nullglob
while true; do
    for tif in "$INPUT_DIR"/*.tif; do
        process_one "$tif"
    done
    sleep "$POLL_SECONDS"
done
