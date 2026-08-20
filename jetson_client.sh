#!/usr/bin/env bash
# USAGE:
#   ./jetson_client.sh list
#   ./jetson_client.sh get SCENE_NAME
#   ./jetson_client.sh get-all
#   ./jetson_client.sh send-coords coords.json [name]
#   ./jetson_client.sh list-images
#   ./jetson_client.sh image-footprints
#   ./jetson_client.sh process IMAGE_NAME          (BAKLAVA_BACKEND=cpp|legacy)
#   ./jetson_client.sh overview SCENE_NAME
set -euo pipefail

JETSON_HOST="${JETSON_HOST:-10.11.250.25}"
JETSON_PORT="${JETSON_PORT:-8080}"
JETSON_URL="http://$JETSON_HOST:$JETSON_PORT"
JETSON_TOKEN="${BAKLAVA_TOKEN:-}"
DEST_DIR="${BAKLAVA_DEST_DIR:-/Users/martinvalentinov/Desktop/scenes}"

mkdir -p "$DEST_DIR"

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

  image-footprints)
    curl_auth -f "$JETSON_URL/images/footprints"
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

  *)
    echo "usage: $0 {list|get NAME|get-all|send-coords FILE [name]|list-images|" \
         "process IMAGE_NAME|overview NAME}" >&2
    exit 64
    ;;
esac
