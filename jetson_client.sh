#!/usr/bin/env bash
# USAGE:
#   ./jetson_client.sh list
#   ./jetson_client.sh get SCENE_NAME
#   ./jetson_client.sh get-all
#   ./jetson_client.sh send-coords coords.json [name]
#   ./jetson_client.sh list-images
#   ./jetson_client.sh process IMAGE_NAME          (BAKLAVA_BACKEND=cpp|legacy)
#   ./jetson_client.sh overview SCENE_NAME
#   ./jetson_client.sh crops-manifest SCENE_NAME
#   ./jetson_client.sh crops SCENE_NAME [--thumbs|--full|--all]
#                                       [--only-detections] [--tar]
set -euo pipefail

JETSON_HOST="${JETSON_HOST:-10.11.250.25}"
JETSON_PORT="${JETSON_PORT:-8080}"
JETSON_URL="http://$JETSON_HOST:$JETSON_PORT"
JETSON_TOKEN="${BAKLAVA_TOKEN:-}"
DEST_DIR="${BAKLAVA_DEST_DIR:-/Users/martinvalentinov/Desktop/scenes}"

mkdir -p "$DEST_DIR"

# On Windows, a bare "python3" (or "python") often resolves to the Microsoft
# Store's placeholder stub rather than a real interpreter, even when a real
# one is installed under a different name on PATH. Probe candidates and pick
# the first one that actually runs, instead of assuming "python3" works.
PYTHON=""
for candidate in python3 python py; do
    if command -v "$candidate" >/dev/null 2>&1 && "$candidate" --version >/dev/null 2>&1; then
        PYTHON="$candidate"
        break
    fi
done
if [[ -z "$PYTHON" ]]; then
    echo "no working python interpreter found (tried python3, python, py)" >&2
    exit 1
fi

curl_auth() {
    local args=(-sS)
    if [[ -n "$JETSON_TOKEN" ]]; then
        args+=(-H "Authorization: Bearer $JETSON_TOKEN")
    fi
    curl "${args[@]}" "$@"
}

url_encode() {
    "$PYTHON" -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1], safe=''))" "$1"
}

cmd="${1:-}"
shift || true

case "$cmd" in
  list)
    curl_auth -f "$JETSON_URL/list"
    ;;

  get)
    name="${1:?usage: jetson_client.sh get SCENE_NAME}"
    out="$DEST_DIR/$name.json"
    if curl_auth -f "$JETSON_URL/scenes/$(url_encode "$name")" -o "$out"; then
        echo "saved -> $out"
    else
        rm -f "$out"
        echo "failed to fetch '$name' (scene may not be ready yet)" >&2
        exit 1
    fi
    ;;

  get-all)
    if curl_auth -f "$JETSON_URL/scenes-all" | tar -xf - -C "$DEST_DIR"; then
        echo "all available jsons extracted -> $DEST_DIR"
    else
        echo "failed to fetch jsons (none available yet?)" >&2
        exit 1
    fi
    ;;

  send-coords)
    file="${1:?usage: jetson_client.sh send-coords coords.json [name]}"
    name="${2:-}"
    if [[ ! -f "$file" ]]; then
        echo "no such file: $file" >&2
        exit 1
    fi
    url="$JETSON_URL/coords"
    if [[ -n "$name" ]]; then
        url="$url?name=$(url_encode "$name")"
    fi
    if curl_auth -f -X POST --data-binary @"$file" -H "Content-Type: application/json" "$url" > /dev/null; then
        echo "coords sent."
    else
        echo "failed to send coords" >&2
        exit 1
    fi
    ;;

  list-images)
    curl_auth -f "$JETSON_URL/images"
    ;;

  process)
    name="${1:?usage: jetson_client.sh process IMAGE_NAME}"
    backend="${BAKLAVA_BACKEND:-cpp}"
    response="$(curl_auth -s -w '\n%{http_code}' -X POST \
        "$JETSON_URL/images/$(url_encode "$name")/process?backend=$(url_encode "$backend")")"
    code="${response##*$'\n'}"
    payload="${response%$'\n'*}"
    if [[ "$code" == "200" ]]; then
        "$PYTHON" -c "import json,sys; print(json.loads(sys.argv[1]).get('message',''))" "$payload"
    else
        "$PYTHON" -c "import json,sys; print(json.loads(sys.argv[1]).get('error','request failed'), file=sys.stderr)" "$payload"
        exit 1
    fi
    ;;

  overview)
    name="${1:?usage: jetson_client.sh overview SCENE_NAME}"
    out="$DEST_DIR/$name.overview.jpg"
    if curl_auth -f "$JETSON_URL/scenes/$(url_encode "$name")/overview" -o "$out"; then
        echo "saved -> $out"
    else
        rm -f "$out"
        echo "no overview for '$name'" >&2
        exit 1
    fi
    ;;

  crops-manifest)
    name="${1:?usage: jetson_client.sh crops-manifest SCENE_NAME}"
    dir="$DEST_DIR/$name-crops"
    mkdir -p "$dir"
    if curl_auth -f "$JETSON_URL/scenes/$(url_encode "$name")/crops" -o "$dir/manifest.json"; then
        echo "manifest -> $dir/manifest.json"
    else
        rm -f "$dir/manifest.json"
        echo "no crops for '$name'" >&2
        exit 1
    fi
    ;;

  crops)
    name="${1:?usage: jetson_client.sh crops SCENE_NAME [--thumbs|--full|--all] [--only-detections] [--tar]}"
    shift
    tier="thumb"
    only=""
    use_tar=0
    while [[ $# -gt 0 ]]; do
        case "$1" in
            --thumbs)          tier="thumb"; shift ;;
            --full)            tier="full";  shift ;;
            --all)             tier="all";   shift ;;
            --only-detections) only="detections"; shift ;;
            --tar)             use_tar=1; shift ;;
            *) echo "unknown option $1" >&2; exit 64 ;;
        esac
    done

    dir="$DEST_DIR/$name-crops"
    mkdir -p "$dir"
    enc_name="$(url_encode "$name")"

    if (( use_tar )); then
        url="$JETSON_URL/scenes/$enc_name/crops.tar?tier=$tier"
        [[ -n "$only" ]] && url="$url&only=$only"
        if curl_auth -f "$url" | tar -xf - -C "$dir"; then
            echo "extracted -> $dir"
        else
            echo "failed to fetch crops for '$name'" >&2
            exit 1
        fi
        exit 0
    fi

    if ! curl_auth -f "$JETSON_URL/scenes/$enc_name/crops" -o "$dir/manifest.json"; then
        rm -f "$dir/manifest.json"
        echo "no crops for '$name' (scene may not be processed yet, or was run" \
             "through the legacy backend)" >&2
        exit 1
    fi

    plan="$("$PYTHON" - "$dir" "$tier" "$only" <<'PY'
import json, os, sys, zlib
root, tier, only = sys.argv[1], sys.argv[2], sys.argv[3]
with open(os.path.join(root, "manifest.json")) as fh:
    m = json.load(fh)

wanted = []
for c in m.get("crops", []):
    if only == "detections" and not c.get("detections"):
        continue
    if tier in ("thumb", "all") and c.get("thumb"):
        wanted.append((c["thumb"], c.get("thumb_bytes", 0), c.get("thumb_crc32", 0)))
    if tier in ("full", "all"):
        wanted.append((c["file"], c.get("bytes", 0), c.get("crc32", 0)))

have = 0
for rel, nbytes, crc in wanted:
    path = os.path.join(root, rel)
    if crc and os.path.isfile(path) and os.path.getsize(path) == nbytes:
        with open(path, "rb") as fh:
            if zlib.crc32(fh.read()) & 0xFFFFFFFF == crc:
                have += 1
                continue
    print(f"FETCH\t{rel}\t{nbytes}\t{crc}")
print(f"STATS\t{len(wanted)}\t{have}")
PY
)"

    total=$(printf '%s\n' "$plan" | awk -F'\t' '$1=="STATS"{print $2}')
    have=$(printf  '%s\n' "$plan" | awk -F'\t' '$1=="STATS"{print $3}')
    got=0; failed=0

    while IFS=$'\t' read -r tag rel want crc; do
        [[ "$tag" != "FETCH" ]] && continue
        dest="$dir/$rel"
        mkdir -p "$(dirname "$dest")"
        part="$dest.$crc.part"
        if curl_auth -f -C - "$JETSON_URL/scenes/$enc_name/crops/$rel" -o "$part"; then
            if "$PYTHON" -c 'import sys,zlib; sys.exit(0 if zlib.crc32(open(sys.argv[1],"rb").read()) & 0xFFFFFFFF == int(sys.argv[2]) else 1)' "$part" "$crc"; then
                mv "$part" "$dest"
                got=$((got + 1))
            else
                rm -f "$part"
                failed=$((failed + 1))
                echo "failed: $rel (checksum mismatch; discarded, refetch on the next run)" >&2
            fi
        else
            failed=$((failed + 1))
            echo "failed: $rel (will resume on the next run)" >&2
        fi
    done <<< "$plan"

    echo "$dir: $total in manifest, $have already local and verified," \
         "$got fetched, $failed failed"
    (( failed == 0 ))
    ;;

  *)
    echo "usage: $0 {list|get NAME|get-all|send-coords FILE [name]|list-images|" \
         "process IMAGE_NAME|overview NAME|crops NAME [--thumbs|--full|--all]" \
         "[--only-detections] [--tar]}" >&2
    exit 64
    ;;
esac
