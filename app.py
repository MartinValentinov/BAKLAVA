import math
import random

from flask import Flask, jsonify, render_template

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

    "link_ais_archive": "https://supm.online/ais/",

    "msg_scenes_failed":  "Could not load the scenes. Try again.",
    "msg_scene_failed":   "Could not load that scene. Try again.",
    "msg_pick_scene":     "Pick a scene on the map",
    "msg_pick_scene_off": "Scene selection cancelled",
    "msg_no_sar":         "This scene has no SAR overlay yet",
    "msg_sar_needs_scene": "Select a scene first",

    "map_default_lat": 42.0,
    "map_default_lon": 20.0,
    "map_default_zoom": 5,
}


SCENE_BLUEPRINTS = [
    {
        "id": "TYR_20240311",
        "label": "Tyrrhenian Sea - 11 Mar",
        "lat": 41.05, "lon": 12.35,
        "width_deg": 2.0, "height_deg": 1.7,
        "rotation_deg": 28,
        "total": 70, "dark": 50,
    },
    {
        "id": "AEG_20240309",
        "label": "Aegean Sea - 9 Mar",
        "lat": 37.60, "lon": 25.30,
        "width_deg": 2.4, "height_deg": 1.9,
        "rotation_deg": -14,
        "total": 46, "dark": 18,
    },
    {
        "id": "BLS_20240307",
        "label": "Western Black Sea - 7 Mar",
        "lat": 43.35, "lon": 30.40,
        "width_deg": 2.6, "height_deg": 2.0,
        "rotation_deg": 9,
        "total": 58, "dark": 31,
    },
    {
        "id": "ADR_20240305",
        "label": "Southern Adriatic - 5 Mar",
        "lat": 41.90, "lon": 18.20,
        "width_deg": 2.1, "height_deg": 1.8,
        "rotation_deg": -35,
        "total": 33, "dark": 7,
    },
]

DEMO_SHIP_NAMES = [
    "MV Adriatic Star", "MV Kalypso", "MT Pontos", "MV Danubia",
    "MT Levant Trader", "MV Zephyros", "MV Ionian Pearl", "MT Bosphorus",
    "MV Sirena", "MT Aegean Dawn", "MV Thalassa", "MV Nordic Ember",
]

DEMO_SHIP_TYPES = ["Cargo", "Tanker", "Fishing", "Container", "Bulk carrier", "Tug"]


def _rotate(dx, dy, degrees):
    angle = math.radians(degrees)
    return (dx * math.cos(angle) - dy * math.sin(angle),
            dx * math.sin(angle) + dy * math.cos(angle))


def _scene_corners(blueprint):
    half_w = blueprint["width_deg"] / 2
    half_h = blueprint["height_deg"] / 2

    upright = [(-half_w, -half_h), (half_w, -half_h),
               (half_w, half_h), (-half_w, half_h)]

    corners = []
    for dx, dy in upright:
        rx, ry = _rotate(dx, dy, blueprint["rotation_deg"])
        corners.append([
            round(blueprint["lat"] + ry, 6),
            round(blueprint["lon"] + rx / math.cos(math.radians(blueprint["lat"])), 6),
        ])
    return corners


def _make_vessels(blueprint):
    rng = random.Random(blueprint["id"])

    half_w = blueprint["width_deg"] / 2
    half_h = blueprint["height_deg"] / 2
    lat_scale = math.cos(math.radians(blueprint["lat"]))

    vessels = []
    for index in range(blueprint["total"]):
        is_dark = index < blueprint["dark"]

        dx = rng.uniform(-half_w, half_w) * 0.88
        dy = rng.uniform(-half_h, half_h) * 0.88
        rx, ry = _rotate(dx, dy, blueprint["rotation_deg"])

        vessels.append({
            "id": f"{blueprint['id']}-{index + 1:03d}",
            "lat": round(blueprint["lat"] + ry, 6),
            "lon": round(blueprint["lon"] + rx / lat_scale, 6),
            "dark": is_dark,
            "name": (f"Dark contact {index + 1:02d}" if is_dark
                     else rng.choice(DEMO_SHIP_NAMES)),
            "mmsi": "" if is_dark else str(rng.randint(200000000, 279999999)),
            "type": "Unknown" if is_dark else rng.choice(DEMO_SHIP_TYPES),
            "length_m": rng.randint(38, 295),
            "heading_deg": rng.randint(0, 359),
            "speed_kn": round(rng.uniform(0.0, 18.5), 1),
            "detected_at": "2024-03-11 04:17 UTC",
            "confidence": round(rng.uniform(0.62, 0.99), 2),
        })

    return vessels


def build_scene_list():
    scenes = []
    for blueprint in SCENE_BLUEPRINTS:
        scenes.append({
            "id": blueprint["id"],
            "label": blueprint["label"],
            "corners": _scene_corners(blueprint),
            "has_sar": False,
        })
    return scenes


def build_scene_detail(scene_id):
    blueprint = next((b for b in SCENE_BLUEPRINTS if b["id"] == scene_id), None)
    if blueprint is None:
        return None

    vessels = _make_vessels(blueprint)

    return {
        "id": blueprint["id"],
        "label": blueprint["label"],
        "corners": _scene_corners(blueprint),
        "totals": {
            "total": len(vessels),
            "dark": sum(1 for vessel in vessels if vessel["dark"]),
        },
        "vessels": vessels,
        "sar_overlay": None,
    }


app = Flask(__name__)


@app.route("/")
def home():
    return render_template("index.html", settings=APP_SETTINGS)


@app.route("/api/scenes")
def api_scenes():
    return jsonify({"scenes": build_scene_list()})


@app.route("/api/scenes/<scene_id>")
def api_scene(scene_id):
    scene = build_scene_detail(scene_id)
    if scene is None:
        return jsonify({"error": "Unknown scene"}), 404
    return jsonify(scene)


if __name__ == "__main__":
    app.run(debug=True, port=5000)
