#!/usr/bin/env bash
#   ./run.sh detect  data/input/scene.SAFE -o data/output  # raw L1 -> ships
#   ./run.sh detect  data/input/scene.tif  -o data/output  # already-L2 -> ships
#   ./run.sh detect  data/input/scene.SAFE -o out --save-l2
#   ./run.sh detect  data/input/scene.tif  -o out --save-render
#   ./run.sh detect  data/input/scene.tif  -o out --no-crops
#   ./run.sh shell

set -euo pipefail

PROJ=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
IMAGE=${IMAGE:-baklava-ship-detect:latest}
DATA="$PROJ/data"
ONNX_NAME=${ONNX_NAME:-model_b16_640.onnx}
ENGINE_NAME=${ENGINE_NAME:-model_b16_640.container.engine}
DEM_NAME=${DEM_NAME:-dem_full.tif}
GEOID_NAME=${GEOID_NAME:-egm96.tif}
OVERVIEW_MAX=${OVERVIEW_MAX:-4096}
BOX_THICKNESS=${BOX_THICKNESS:-0}
CFAR_THRESH=${CFAR_THRESH:-50}
# Measured on the Bosphorus/Marmara scene (S1D ...155853), which has real
# 30+ ship anchorages 240-1000 m off the beach.
#
# What was ruled out first, with numbers, because each looked like the obvious
# culprit and none of them was:
#   CFAR      --cfar 0 infers 386 tiles instead of 314 and yields the SAME 412
#             detections. The tiles it skips are genuinely empty.
#   land mask --buffer-m 0 moves exactly one tile out of "fully land" (980->979).
#   NMS       raising --nms-iou 0.30->0.75 adds 96 boxes, but 140 of them then
#             sit within 30 m of a neighbour: duplicate boxes on one hull, not
#             extra ships. At 0.30 that count is 0. NMS is doing its job.
#
# What it actually is: coastal vessels are smaller (median 142 m within 500 m of
# shore vs 243 m offshore), the network is less certain about small targets, and
# a global confidence gate tuned on open water cuts them. The 49 near-shore
# detections in the 0.05-0.40 band have ZERO neighbours within 60 m and a median
# length of 175 m -- distinct, ship-sized, and real.
#
# 0.30 is where the trade stops paying: +31 ships for +4 land-clutter drops
# (7.8:1). At 0.25 that is 1.8:1 and at 0.20 it inverts to 0.8:1.
# Near-shore recovery at 0.30: 38->46 within 500 m, 83->99 within 1 km.
CONF_THRESH=${CONF_THRESH:-0.30}

TTY=()
[[ -t 0 && -t 1 ]] && TTY=(-it)

# `docker compose up -d` (see docker-compose.yml) keeps a container of $IMAGE
# alive on "sleep infinity" so a run only pays GPU-runtime/model-load cost
# once, not on every detect call. run_exec() execs into it when present;
# otherwise it falls back to the old one-shot `docker run --rm` unchanged, so
# nothing breaks on a host where the compose service was never started.
PERSIST_NAME=${PERSIST_NAME:-sar-ship-detect}
COMPOSE_FILE="$PROJ/docker-compose.yml"
SERVE_DIR=${SERVE_DIR:-$DATA/.serve}
SERVE_ROOT=${SERVE_ROOT:-/data/code/Baklava}
SERVE_OPEN_TIMEOUT=${SERVE_OPEN_TIMEOUT:-1}

serve_usable() {
    [[ -p "$SERVE_DIR/req" && -p "$SERVE_DIR/resp" ]] || return 1
    [[ "$(id -u)" == "${SERVE_UID:-0}" ]] || return 1
    local p
    for p in "$@"; do
        [[ "$p" == "$SERVE_ROOT" || "$p" == "$SERVE_ROOT"/* ]] || return 1
    done
    return 0
}

# Returns 0 when the resident detector ran the job, and leaves its exit status in
# SERVE_RC; returns 1 only when serve could not be reached at all. Keeping those
# two apart matters: a job that legitimately fails must report its own status,
# not be silently run a second time down the docker exec path.
SERVE_RC=""
run_serve() {
    local log="$SERVE_DIR/job.$$.log"
    local req="" a
    for a in "$@"; do req+=$'\t'"$a"; done
    if ! timeout "$SERVE_OPEN_TIMEOUT" bash -c 'cat > "$1"' _ "$SERVE_DIR/req" \
             <<<"$log$req" 2>/dev/null; then
        rm -f "$log"; return 1
    fi
    local rc; rc=$(timeout "${SERVE_JOB_TIMEOUT:-300}" cat "$SERVE_DIR/resp" 2>/dev/null | head -1)
    cat "$log" >&2
    rm -f "$log"
    [[ "$rc" =~ ^[0-9]+$ ]] || return 1
    SERVE_RC="$rc"
    return 0
}

# The serve process is the container's own entrypoint (see docker-compose.yml:
# /app/serve_supervisor.sh), so `restart: unless-stopped` covers it and a host
# reboot brings it back on its own. The supervisor relaunches the detector if it
# dies, and idles instead of relaunching while $SERVE_DIR/stop exists, which is
# how serve-stop keeps the container alive for the docker exec fallback.
serve_pid() {
    docker exec "$PERSIST_NAME" pgrep -f 'sar_ship_detect --serve' 2>/dev/null | head -1
}

serve_kv() {
    local file=$1 key=$2
    [[ -r "$SERVE_DIR/$file" ]] || return 1
    local v
    v=$(grep -m1 "^$key=" "$SERVE_DIR/$file" 2>/dev/null) || return 1
    printf '%s' "${v#*=}"
}

serve_age() {
    local then=$1 now
    [[ -n "$then" ]] || { printf 'unknown'; return; }
    now=$(date +%s)
    local d=$(( now - then ))
    (( d < 0 )) && d=0
    if   (( d < 60 ));   then printf '%ds' "$d"
    elif (( d < 3600 )); then printf '%dm%02ds' $(( d / 60 )) $(( d % 60 ))
    else printf '%dh%02dm' $(( d / 3600 )) $(( d % 3600 / 60 )); fi
}

serve_wait_ready() {
    local deadline=$(( SECONDS + ${1:-90} )) phase
    while (( SECONDS < deadline )); do
        if [[ -n "$(serve_pid)" ]]; then
            phase=$(serve_kv stats phase || true)
            [[ "$phase" == "ready" || "$phase" == "busy" ]] && return 0
        fi
        sleep 0.5
    done
    return 1
}

PERSIST_ROOTS=()
PERSIST_RUNNING=""
PERSIST_PROBED=0
persist_probe() {
    (( PERSIST_PROBED )) && return 0
    PERSIST_PROBED=1
    local line src dst first=1
    while IFS= read -r line; do
        if (( first )); then PERSIST_RUNNING="$line"; first=0; continue; fi
        read -r src dst <<<"$line"
        [[ -n "$src" && "$src" == "$dst" ]] && PERSIST_ROOTS+=("$src")
    done < <(docker inspect \
                 -f '{{.State.Running}}{{"\n"}}{{range .Mounts}}{{.Source}} {{.Destination}}{{"\n"}}{{end}}' \
                 "$PERSIST_NAME" 2>/dev/null)
    return 0
}

persistent_up() {
    persist_probe
    [[ "$PERSIST_RUNNING" == "true" ]]
}

persist_roots() {
    persist_probe
    (( ${#PERSIST_ROOTS[@]} ))
}

inside_persist() {
    local p=$1 root
    for root in "${PERSIST_ROOTS[@]}"; do
        [[ "$p" == "$root" || "$p" == "$root"/* ]] && return 0
    done
    return 1
}

run_container() {
    local entry=$1; shift
    exec docker run --rm "${TTY[@]}" \
        --runtime nvidia \
        -e NVIDIA_VISIBLE_DEVICES=all \
        -e NVIDIA_DRIVER_CAPABILITIES=all \
        --ipc=host \
        -u "$(id -u):$(id -g)" \
        --entrypoint "$entry" \
        --label project=baklava-ship-detect \
        "${MOUNTS[@]}" \
        "$IMAGE" "$@"
}

run_exec() {
    local entry=$1; shift
    local envs=()
    [[ -n "${DUMP_CELLS:-}" ]] && envs=(-e "DUMP_CELLS=$DUMP_CELLS")
    exec docker exec "${TTY[@]}" "${envs[@]}" -u "$(id -u):$(id -g)" "$PERSIST_NAME" "$entry" "$@"
}

cmd=${1:-help}; shift || true

case "$cmd" in
  detect)
    [[ $# -ge 1 ]] || { echo "usage: ./run.sh detect SCENE[.tif|.SAFE|.zip] -o OUTDIR [flags...]" >&2; exit 64; }
    scene=$(cd "$(dirname "$1")" && pwd)/$(basename "$1"); shift
    outdir="."
    extra=()
    want_render=0
    want_crops=0
    want_overview=1
    want_l2=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            -o) outdir=$2; shift 2 ;;
            --save-render) want_render=1; shift ;;
            --save-l2) want_l2=1; shift ;;
            --no-crops) want_crops=0; shift ;;
            --no-overview) want_overview=0; shift ;;
            *) extra+=("$1"); shift ;;
        esac
    done
    mkdir -p "$outdir"
    outdir=$(cd "$outdir" && pwd)
    stem=$(basename "$scene")
    stem=${stem%.tif}; stem=${stem%.tiff}; stem=${stem%.SAFE}; stem=${stem%.safe}; stem=${stem%.zip}

    case "$(basename "$scene")" in
        *.SAFE|*.safe|*.SAFE/|*.zip|*.ZIP) is_l1=1 ;;
        *) is_l1=0 ;;
    esac

    USE_PERSIST=0
    if persistent_up && persist_roots \
       && inside_persist "$scene" && inside_persist "$outdir" && inside_persist "$DATA"; then
        USE_PERSIST=1
    fi

    if (( USE_PERSIST )); then
        # The persistent container mounts the whole /data/code/Baklava tree at
        # its real host path (see docker-compose.yml), so no /scene_in or
        # /scene_out indirection is needed -- the host paths are already valid
        # inside it, .SAFE directories included (a directory can't be hard-
        # linked the way a flat raster is, which is why that indirection
        # existed for run_container's per-call bind mount in the first place).
        SCENE_IN_ARG="$scene"
        SCENE_OUT="$outdir"
        MODEL_DIR="$DATA/model"
        GSHHG_DIR="$DATA/GSHHS_shp/f"
        DEM_DIR="$DATA/dem"
    else
        SCENE_IN_ARG="/scene_in/$(basename "$scene")"
        SCENE_OUT="/scene_out"
        MODEL_DIR="/data/model"
        GSHHG_DIR="/data/GSHHS_shp/f"
        DEM_DIR="/data/dem"
        MOUNTS=(
            -v "$DATA/model:/data/model"
            -v "$DATA/GSHHS_shp/f:/data/GSHHS_shp/f:ro"
            -v "$(dirname "$scene"):/scene_in:ro"
            -v "$outdir:/scene_out"
        )
    fi

    (( want_render ))   && extra+=(--out-jpg "$SCENE_OUT/${stem}.jpg")
    (( want_overview )) && extra+=(--out-overview "$SCENE_OUT/${stem}_overview.jpg" --overview-max "$OVERVIEW_MAX" --box-thickness "$BOX_THICKNESS")
    (( CFAR_THRESH > 0 )) && extra+=(--cfar "$CFAR_THRESH")
    # Only inject the default if the caller did not pass their own --conf --
    # an explicit flag on the command line must win, not be silently
    # overwritten by a later occurrence in argv.
    has_conf=0
    for a in "${extra[@]}"; do [[ "$a" == "--conf" ]] && has_conf=1; done
    (( has_conf )) || extra+=(--conf "$CONF_THRESH")
    (( want_crops ))    && extra+=(--out-crops "$SCENE_OUT/${stem}_crops")
    (( want_l2 ))       && extra+=(--out-l2 "$SCENE_OUT/${stem}_L2.tif")

    GSHHG_ARGS=()
    for lvl in L1 L2 L3 L4 L5 L6; do
        GSHHG_ARGS+=(--gshhg "$GSHHG_DIR/GSHHS_f_${lvl}.shp")
    done

    if (( is_l1 )); then
        [[ -f "$DATA/dem/$DEM_NAME" ]] || {
            echo "no DEM at $DATA/dem/$DEM_NAME" >&2
            echo "A raw .SAFE needs one for terrain correction. Either put a DEM" >&2
            echo "there (see MANUAL.md \u00a7 'Preparing a DEM'), or pass an" >&2
            echo "already-terrain-corrected .tif instead." >&2
            exit 1
        }
        (( USE_PERSIST )) || MOUNTS+=(-v "$DATA/dem:/data/dem:ro")
        input=(--safe "$SCENE_IN_ARG" --pol "${POL:-VH}"
               --dem "$DEM_DIR/$DEM_NAME")
        if [[ -f "$DATA/dem/$GEOID_NAME" ]]; then
            input+=(--geoid "$DEM_DIR/$GEOID_NAME")
        else
            echo "warning: no geoid grid at $DATA/dem/$GEOID_NAME." >&2
            echo "         SRTM heights are orthometric, and skipping the geoid is a" >&2
            echo "         measured ~4 px geolocation error in the Black Sea." >&2
            echo "         Proceeding only because you may have an ellipsoidal DEM." >&2
            input+=(--dem-is-ellipsoidal)
        fi
    else
        input=(--tif "$SCENE_IN_ARG")
    fi

    DETECT_ARGS=(
        "${input[@]}"
        --onnx "$MODEL_DIR/$ONNX_NAME"
        --engine "$MODEL_DIR/$ENGINE_NAME"
        "${GSHHG_ARGS[@]}"
        --out-json "$SCENE_OUT/${stem}.json"
        "${extra[@]}"
    )
    if (( USE_PERSIST )) && serve_usable "$scene" "$outdir" "$DATA"; then
        if run_serve "${DETECT_ARGS[@]}"; then exit "$SERVE_RC"; fi
        echo "warning: resident detector did not answer; falling back to docker exec" >&2
    fi
    if (( USE_PERSIST )); then
        run_exec /app/sar_ship_detect "${DETECT_ARGS[@]}"
    else
        run_container /app/sar_ship_detect "${DETECT_ARGS[@]}"
    fi
    ;;
  up)
    mkdir -p "$SERVE_DIR"
    rm -f "$SERVE_DIR/stop"
    docker compose -f "$COMPOSE_FILE" up -d "$@"
    if serve_wait_ready 120; then
        echo "serve: ready (pid $(serve_pid), auto-restarts with the container)"
    else
        echo "serve: did not report ready within 120s; see '$0 serve-log'" >&2
        exit 1
    fi
    ;;
  down|stop)
    exec docker compose -f "$COMPOSE_FILE" down "$@"
    ;;
  detector-start)
    # Same effect as 'up' but with plain `docker start`, because the caller may
    # be the listener container: it has the docker CLI and the socket, but not
    # the compose plugin. Needs the container to exist already, which '$0 up'
    # on the host does once.
    docker inspect "$PERSIST_NAME" >/dev/null 2>&1 || {
        echo "no container '$PERSIST_NAME'; create it once with '$0 up' on the host" >&2
        exit 1
    }
    mkdir -p "$SERVE_DIR"
    rm -f "$SERVE_DIR/stop"
    persistent_up || docker start "$PERSIST_NAME" >/dev/null
    if serve_wait_ready "${DETECTOR_START_TIMEOUT:-120}"; then
        echo "detector: ready (pid $(serve_pid))"
    else
        echo "detector: did not report ready in time; see '$0 serve-log'" >&2
        exit 1
    fi
    ;;
  detector-stop)
    if docker inspect "$PERSIST_NAME" >/dev/null 2>&1; then
        docker stop -t "${DETECTOR_STOP_TIMEOUT:-15}" "$PERSIST_NAME" >/dev/null 2>&1 || true
        echo "detector: stopped (GPU released; detect falls back to a one-shot container)"
    else
        echo "detector: no container '$PERSIST_NAME' to stop"
    fi
    ;;
  serve-start)
    persistent_up || { echo "'$PERSIST_NAME' is not running; use '$0 up'" >&2; exit 1; }
    rm -f "$SERVE_DIR/stop"
    if serve_wait_ready 120; then
        echo "serve: ready (pid $(serve_pid))"
    else
        echo "serve: did not report ready within 120s; see '$0 serve-log'" >&2
        exit 1
    fi
    ;;
  serve-stop)
    mkdir -p "$SERVE_DIR"
    touch "$SERVE_DIR/stop"
    docker exec "$PERSIST_NAME" pkill -TERM -f 'sar_ship_detect --serve' >/dev/null 2>&1 || true
    for _ in $(seq 1 40); do
        [[ -z "$(serve_pid)" ]] && break
        sleep 0.25
    done
    if [[ -z "$(serve_pid)" ]]; then
        echo "serve: stopped (container still up, detect falls back to docker exec)"
    else
        echo "serve: still running after SIGTERM; see '$0 serve-log'" >&2
        exit 1
    fi
    ;;
  serve-restart)
    persistent_up || { echo "'$PERSIST_NAME' is not running; use '$0 up'" >&2; exit 1; }
    rm -f "$SERVE_DIR/stop"
    docker exec "$PERSIST_NAME" pkill -TERM -f 'sar_ship_detect --serve' >/dev/null 2>&1 || true
    sleep 1
    if serve_wait_ready 120; then
        echo "serve: ready (pid $(serve_pid))"
    else
        echo "serve: did not come back within 120s; see '$0 serve-log'" >&2
        exit 1
    fi
    ;;
  serve-log)
    [[ $# -gt 0 ]] || set -- -n 200
    exec tail "$@" "$SERVE_DIR/serve.log"
    ;;
  serve-status|status)
    rc=0
    if persistent_up; then
        printf 'container   %-10s %s\n' up \
            "$(docker inspect -f '{{.State.StartedAt}}' "$PERSIST_NAME" 2>/dev/null)"
    else
        printf 'container   %-10s %s\n' down "start it with: $0 up"
        rc=1
    fi

    sup_phase=$(serve_kv state phase || echo unknown)
    printf 'supervisor  %-10s restarts=%s\n' "$sup_phase" "$(serve_kv state restarts || echo ?)"

    pid=$(serve_pid)
    if [[ -n "$pid" ]]; then
        printf 'serve       %-10s pid=%s up=%s jobs=%s last=rc%s/%sms %s ago\n' \
            "$(serve_kv stats phase || echo '?')" "$pid" \
            "$(serve_age "$(serve_kv stats started || true)")" \
            "$(serve_kv stats jobs || echo ?)" \
            "$(serve_kv stats last_rc || echo -)" \
            "$(serve_kv stats last_ms || echo -)" \
            "$(serve_age "$(serve_kv stats updated || true)")"
    elif [[ -e "$SERVE_DIR/stop" ]]; then
        printf 'serve       %-10s %s\n' stopped "start it with: $0 serve-start"
        rc=1
    else
        printf 'serve       %-10s %s\n' down "supervisor should relaunch it within a second"
        rc=1
    fi

    if [[ -p "$SERVE_DIR/req" && -p "$SERVE_DIR/resp" ]]; then
        printf 'fifos       %-10s %s\n' ok "$SERVE_DIR"
    else
        printf 'fifos       %-10s %s\n' missing "$SERVE_DIR"
        rc=1
    fi

    if [[ "$(id -u)" == "${SERVE_UID:-0}" ]]; then
        printf 'fast path   %-10s this shell (uid %s) uses the resident process\n' yes "$(id -u)"
    else
        printf 'fast path   %-10s uid %s != serve uid %s, so detect uses docker exec\n' \
            no "$(id -u)" "${SERVE_UID:-0}"
    fi
    exit $rc
    ;;
  shell|bash)
    MOUNTS=(
        -v "$DATA:/data"
    )
    run_container bash "$@"
    ;;
  help|*)
    cat >&2 <<USAGE
sar-ship-detect (option B) host wrapper

  ./run.sh detect SCENE[.tif|.SAFE|.zip] -o OUTDIR
                          [--save-render] [--save-l2] [--no-crops]
                          [--no-overview] [flags...]
                          run the detector; other flags forward to
                          sar_ship_detect (--conf, --buffer-m, --crop-size, ...)
                          writes <stem>.json, <stem>_overview.jpg and
                          <stem>_crops/ by default
                          a .SAFE/.zip is processed L1->L2 on the GPU first
  ./run.sh shell          interactive bash, /data/ mounted whole
  ./run.sh up             start the container; its entrypoint is the serve
                          supervisor, so the resident detector comes up with it
                          and again after a crash or a host reboot
  ./run.sh down           stop the container
  ./run.sh status         one screen: container, supervisor, serve, fifos
  ./run.sh serve-status   same as status; exits 1 when serve is not usable
  ./run.sh serve-stop     stop the resident detector, keep the container up
  ./run.sh serve-start    undo serve-stop
  ./run.sh serve-restart  hand back a freshly started detector
  ./run.sh serve-log [-f] the resident detector's own log
  ./run.sh detector-start start the existing container and wait for the model
  ./run.sh detector-stop  stop it and release the GPU
                          these two need no compose plugin, which is what the
                          baklava-listener container calls on its way up and
                          down (see jetson/listener_entrypoint.sh)

env: IMAGE=$IMAGE  ONNX_NAME=$ONNX_NAME  ENGINE_NAME=$ENGINE_NAME
     DEM_NAME=$DEM_NAME  GEOID_NAME=$GEOID_NAME  POL=\${POL:-VH}
note: first detect with a missing \$ENGINE_NAME builds it (~10-20 min, one-time).
note: a .SAFE input needs data/dem/$DEM_NAME and data/dem/$GEOID_NAME.
note: with '$PERSIST_NAME' up, detect execs into it and SCENE and OUTDIR must
      both live under the tree docker-compose.yml mounts; anything outside it
      falls back to a one-shot container automatically.
USAGE
    exit 1 ;;
esac
