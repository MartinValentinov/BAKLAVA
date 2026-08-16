#!/usr/bin/env bash
set -euo pipefail

INPUT_DIR="${INPUT_DIR:-/data/input}"
OUTPUT_DIR="${OUTPUT_DIR:-/data/output}"
MODEL_DIR="${MODEL_DIR:-/data/model}"
GSHHG_DIR="${GSHHG_DIR:-/data/GSHHS_shp/f}"
DEM_DIR="${DEM_DIR:-/data/dem}"
DEM="${DEM:-$DEM_DIR/dem.tif}"
GEOID="${GEOID:-$DEM_DIR/egm96.tif}"
POL="${POL:-VH}"
ONNX="${ONNX:-$MODEL_DIR/model_b16_640.onnx}"
ENGINE="${ENGINE:-$MODEL_DIR/model_b16_640.engine}"
POLL_SECONDS="${POLL_SECONDS:-10}"
DEBUG_JPG="${DEBUG_JPG:-0}"
CROPS="${CROPS:-1}"
OVERVIEW="${OVERVIEW:-1}"
CROP_SIZE="${CROP_SIZE:-1024}"
CROP_QUALITY="${CROP_QUALITY:-80}"
CROP_THUMB="${CROP_THUMB:-256}"
BIN="${BIN:-/app/sar_ship_detect}"

GSHHG_ARGS=()
for lvl in L1 L2 L3 L4 L5 L6; do
    GSHHG_ARGS+=(--gshhg "$GSHHG_DIR/GSHHS_f_${lvl}.shp")
done

mkdir -p "$OUTPUT_DIR"

if [[ ! -f "$ENGINE" ]]; then
    echo "[service] no cached engine at $ENGINE -- building before watching $INPUT_DIR"
fi

process_one() {
    local scene="$1"
    local stem
    stem="$(basename "$scene")"
    stem="${stem%.tif}"; stem="${stem%.tiff}"
    stem="${stem%.SAFE}"; stem="${stem%.safe}"; stem="${stem%.zip}"
    local out_json="$OUTPUT_DIR/${stem}.json"
    local marker="$OUTPUT_DIR/.${stem}.processing"

    [[ -f "$out_json" ]] && return 0
    [[ -f "$marker" ]] && return 0
    touch "$marker"

    local input=()
    case "$scene" in
        *.SAFE|*.safe|*.zip|*.ZIP)
            if [[ ! -f "$DEM" ]]; then
                echo "[service] FAILED $scene -- no DEM at $DEM, needed for terrain correction" >&2
                rm -f "$marker"
                return 0
            fi
            input=(--safe "$scene" --pol "$POL" --dem "$DEM")
            if [[ -f "$GEOID" ]]; then
                input+=(--geoid "$GEOID")
            else
                echo "[service] WARNING no geoid at $GEOID; assuming the DEM is ellipsoidal" >&2
                input+=(--dem-is-ellipsoidal)
            fi
            ;;
        *)
            input=(--tif "$scene")
            ;;
    esac

    local extra=()
    [[ "$DEBUG_JPG" == "1" ]] && extra+=(--out-jpg "$OUTPUT_DIR/${stem}.jpg")
    [[ "$OVERVIEW"  == "1" ]] && extra+=(--out-overview "$OUTPUT_DIR/${stem}_overview.jpg")
    if [[ "$CROPS" == "1" ]]; then
        extra+=(--out-crops "$OUTPUT_DIR/${stem}_crops"
                --crop-size "$CROP_SIZE"
                --crop-quality "$CROP_QUALITY"
                --crop-thumb "$CROP_THUMB")
    fi

    echo "[service] processing $scene"
    if "$BIN" \
        "${input[@]}" \
        --onnx "$ONNX" --engine "$ENGINE" \
        "${GSHHG_ARGS[@]}" \
        --out-json "${out_json}.tmp" \
        "${extra[@]}"; then
        mv "${out_json}.tmp" "$out_json"
        rm -f "$marker"
        echo "[service] done $scene -> $out_json"
    else
        echo "[service] FAILED $scene -- leaving $marker so it isn't silently retried" >&2
        rm -f "${out_json}.tmp"
    fi
}

echo "[service] watching $INPUT_DIR every ${POLL_SECONDS}s"
shopt -s nullglob
while true; do
    for scene in "$INPUT_DIR"/*.tif "$INPUT_DIR"/*.tiff \
                 "$INPUT_DIR"/*.SAFE "$INPUT_DIR"/*.zip; do
        process_one "$scene"
    done
    sleep "$POLL_SECONDS"
done
