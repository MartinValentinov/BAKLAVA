import os
import urllib.error
import urllib.parse
import urllib.request

from flask import Flask, Response, jsonify, render_template, request

APP_SETTINGS = {
    "project_title": "BAKLAVA",
    "project_subtitle": "BAlKan Location Analisys of Vessels with AI",

    "logo_path": "img/logo.svg",

    "loading_animation_path": "anim/loading.svg",
    "loading_text": "Working...",
    "loading_min_ms": 500,

    "btn_select_title": "Select a SAR scene",
    "btn_sar_title": "Show the SAR overlay",
    "toggle_dark_label": "Dark Vessels",

    "stat_total_label": "Total Ships:",
    "stat_dark_label": "Dark Vessels:",

    "ship_details_title": "Ship details:",

    "picker_title": "Scenes",
    "picker_processed_label": "On the map",
    "picker_available_label": "Ready to process",
    "picker_empty": "Nothing on the Jetson yet",
    "picker_no_processed": "No scene has been processed yet",
    "picker_no_available": "Every image on the Jetson has been processed",

    "link_ais_archive": "https://www.marinetraffic.com",
    "link_demo": os.environ.get("BAKLAVA_DEMO_URL", "http://localhost:5001/"),

    "msg_scenes_failed":  "Could not load the scenes. Try again.",
    "msg_scene_failed":   "Could not load that scene. Try again.",
    "msg_pick_scene":     "Pick a scene on the map",
    "msg_pick_scene_off": "Scene selection cancelled",
    "msg_no_sar":         "This scene has no SAR overlay yet",
    "msg_sar_needs_scene": "Select a scene first",
    "msg_images_failed":  "Could not reach the Jetson. Try again.",
    "msg_processing":     "Processing on the Jetson - this can take a while",
    "msg_process_failed": "Processing failed",
    "msg_process_done":   "Processed",

    "confirm_process_title": "Process this scene on the Jetson?",
    "confirm_process_body":  "It runs the full pipeline and can take several minutes.",

    "map_default_lat": 42.0,
    "map_default_lon": 20.0,
    "map_default_zoom": 5,
}

BACKEND_URL = os.environ.get("BAKLAVA_BACKEND_URL", "http://localhost:5080")
BACKEND_TIMEOUT = 150
PROCESS_TIMEOUT = 1800

def _backend_open(path, range_header=None, method="GET", timeout=None):
    req = urllib.request.Request(
        BACKEND_URL + path, data=b"" if method == "POST" else None, method=method)
    if range_header:
        req.add_header("Range", range_header)
    return urllib.request.urlopen(req, timeout=timeout or BACKEND_TIMEOUT)


def _proxy_json(path, method="GET", timeout=None):
    try:
        with _backend_open(path, method=method, timeout=timeout) as resp:
            return Response(resp.read(), status=resp.status, mimetype="application/json")
    except urllib.error.HTTPError as exc:
        return Response(exc.read(), status=exc.code, mimetype="application/json")
    except urllib.error.URLError as exc:
        return jsonify({"error": f"backend unreachable: {exc.reason}"}), 502


def _proxy_binary(path, mimetype):
    try:
        with _backend_open(path, request.headers.get("Range")) as resp:
            headers = {}
            for h in ("Content-Range", "Accept-Ranges", "Content-Length"):
                if h in resp.headers:
                    headers[h] = resp.headers[h]
            return Response(resp.read(), status=resp.status, mimetype=mimetype, headers=headers)
    except urllib.error.HTTPError as exc:
        return Response(exc.read(), status=exc.code, mimetype="application/json")
    except urllib.error.URLError as exc:
        return jsonify({"error": f"backend unreachable: {exc.reason}"}), 502


app = Flask(__name__)


@app.route("/")
def home():
    return render_template("index.html", settings=APP_SETTINGS)


@app.route("/api/scenes")
def api_scenes():
    return _proxy_json("/api/scenes")


@app.route("/api/scenes/available", methods=["POST"])
def api_scenes_available():
    return _proxy_json("/api/scenes/available", method="POST")


@app.route("/api/scenes/<scene_id>/process", methods=["POST"])
def api_scene_process(scene_id):
    return _proxy_json(
        f"/api/scenes/{urllib.parse.quote(scene_id, safe='')}/process",
        method="POST", timeout=PROCESS_TIMEOUT)


@app.route("/api/scenes/<scene_id>")
def api_scene(scene_id):
    return _proxy_json(f"/api/scenes/{urllib.parse.quote(scene_id, safe='')}")


@app.route("/api/scenes/<scene_id>/overview")
def api_scene_overview(scene_id):
    return _proxy_binary(f"/api/scenes/{urllib.parse.quote(scene_id, safe='')}/overview", "image/jpeg")


if __name__ == "__main__":
    app.run(debug=True, port=5000)
