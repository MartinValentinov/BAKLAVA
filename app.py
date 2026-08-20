import functools
import json
import os
import urllib.error
import urllib.parse
import urllib.request

from flask import Flask, Response, jsonify, render_template, request

APP_SETTINGS = {
    "project_title": "BAKLAVA",
    "demo_title": "Pipeline walkthrough",
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
    "link_main": os.environ.get("BAKLAVA_MAIN_URL", "http://127.0.0.1:5000/map"),

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

    "demo_steps": [
        {
            "title": "Read the scene",
            "lead": "Open the .SAFE package the satellite delivered.",
            "body": "Sentinel-1 ships a .SAFE file, a folder containing velocity and position of the satellite. Using the available information we can calculate where the echoes come from.",
            "stages": ["SAFE metadata parse", "orbit table", "slant-range tables"],
            "visual": "footprint",
            "inset": "raw",
            "inset_caption": "The raw VH band, in radar geometry - not a map yet",
        },
        {
            "title": "Lay out the output grid",
            "lead": "Define the final map geometry.",
            "body": "Radar data is recorded in the satellite’s own viewing geometry. We create a north-up UTM grid, a meter-based map grid, with 10 m pixels so each pixel corresponds to a real position on the ground. Its size is calculated from the scene’s geographic footprint.",
            "stages": ["output raster alloc"],
            "visual": "footprint",
        },
        {
            "title": "Load the terrain model",
            "lead": "Radar needs to know the shape of the ground.",
            "body": "A hill can appear displaced in the radar image because of its height and the satellite's viewing angle. We read the DEM (Digital Elevation Model), which provides the ground elevation at each location, to account for terrain height and determine the correct position of the radar data.",
            "stages": ["DEM read", "coarse lattice", "geoid read + apply"],
            "visual": "footprint",
        },
        {
            "title": "Calibrate the radar",
            "lead": "Convert raw measurements into meaningful radar values.",
            "body": "The radar data starts as raw detector measurements rather than physical brightness. We apply the calibration and noise data provided with the Sentinel-1 product to convert them into sigma-nought (σ⁰), a standardized measure of radar backscatter that makes measurements comparable across the scene.",
            "stages": ["measurement read + calibrate"],
            "visual": "footprint",
        },
        {
            "title": "Load the network",
            "lead": "Meanwhile, on another thread.",
            "body": "The TensorRT engine is loaded while the radar scene is being read from storage. Its startup costs milliseconds, but none of them add to the critical path because the scene read takes longer.",
            "stages": ["engine load"],
            "visual": "footprint",
        },
        {
            "title": "Geocode",
            "lead": "Turn radar measurements into a map.",
            "body": "For every output pixel, we solve the range-Doppler equations to determine which radar measurement belongs at that location. The GPU performs this for the entire scene, transforming the radar data from the satellite’s viewing geometry into a north-up map.",
            "stages": ["srgr table", "range-doppler geocode"],
            "visual": "sar",
            "fact": "The image is rotated because the satellite does not fly north-south"
        },
        {
            "title": "Build the land mask",
            "lead": "Removing the ground",
            "body": "We use coastline vectors to identify the land and exclude it from detection, so buildings, piers and other objects on shore do not confuse the AI and get detected as ships.",
            "stages": ["landmask build"],
            "visual": "landmask",
        },
        {
            "title": "Look for candidates",
            "lead": "Filter the scene before detection.",
            "body": "CFAR looks for areas that are unusually bright compared with their local surroundings. Combined with the land mask, it removes most areas that are unlikely to contain ships before sending tiles to the AI.",
            "stages": ["cfar screen", "tile triage"],
            "visual": "sar",
        },
        {
            "title": "Detect",
            "lead": "Detect the ships.",
            "body": "YOLOv8 analyzes the remaining tiles and looks for ships. It draws an oriented box around each detected vessel, giving us its position, size and direction.",
            "stages": ["stream+lane setup", "inference"],
            "visual": "vessels_raw",
        },
        {
            "title": "Remove overlaping detections",
            "lead": "One ship, one detection.",
            "body": "Overlapping tiles can detect the same ship more than once, so we merge duplicate detections. We then remove detections on land or too close to the coastline.",
            "stages": ["rotated NMS", "land filter"],
            "visual": "vessels_final",
        },
        {
            "title": "Cross-reference",
            "lead": "Send the results to the ground station.",
            "body": "The detected ships, their positions, sizes and headings are saved as JSON. We also create a smaller overview image for the browser, while ships with no matching AIS signal are flagged as dark vessels.",
            "stages": ["overview render", "overview reprojection"],
            "visual": "vessels_final",
            "fact": "Dark vessels are contacts with no matching AIS broadcast"
        }
    ],

    "demo_scene_assets": {
        "S1D_IW_GRDH_1SDV_20260802T155853_20260802T155918_003949_007286_1028__cpp": {
            "raw": "img/stages/bosphorus_raw.jpg"
        }
    },

    "demo_aggregate_stages": [
        "parallel load (4 lanes)",
        "L1->L2 total",
        "HOT PATH (post-load)",
        "TOTAL",
        "jetson handler total",
        "detector container startup"
    ],

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


COASTLINE_PATH = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                              "data", "coastline.json")


@functools.lru_cache(maxsize=1)
def _coastline():
    try:
        with open(COASTLINE_PATH) as handle:
            return json.load(handle)
    except OSError:
        return []


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


@app.route("/api/coastline")
def api_coastline():
    try:
        west  = float(request.args["w"])
        south = float(request.args["s"])
        east  = float(request.args["e"])
        north = float(request.args["n"])
    except (KeyError, ValueError):
        return jsonify({"error": "need numeric w, s, e, n"}), 400

    hits = [
        {"type": "Feature", "properties": {"lvl": f["l"]}, "geometry": f["g"]}
        for f in _coastline()
        if f["b"][0] <= east and f["b"][2] >= west
        and f["b"][1] <= north and f["b"][3] >= south
    ]
    return jsonify({"type": "FeatureCollection", "features": hits})


@app.route("/api/last-viewed")
def api_last_viewed():
    return _proxy_json("/api/scenes/last-viewed")


@app.route("/api/last-viewed/<scene_id>", methods=["PUT"])
def api_set_last_viewed(scene_id):
    return _proxy_json(
        f"/api/scenes/last-viewed/{urllib.parse.quote(scene_id, safe='')}", method="PUT")


if __name__ == "__main__":
    app.run(debug=True, host="127.0.0.1",
            port=int(os.environ.get("PORT", "5001")))
