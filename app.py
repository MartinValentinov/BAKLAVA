"""
================================================================================
 BAKLAVA - BAlKan Location Analisys of Vessels with AI
 Web application backend (Flask)
================================================================================

WHAT THIS FILE DOES
-------------------
This is the *server*. The whole interface is now ONE MAP, so the server only
has to answer three things:

  1. Serve the single web page (templates/index.html)   -> GET  /
  2. Serve the list of SAR scenes (the blue boxes)      -> GET  /api/scenes
  3. Serve the vessels found inside one scene           -> GET  /api/scenes/<id>

The browser (static/js/baklava.js) asks this server for data and draws it.
Right now the data is FAKE (see SECTION 2) so you can click through the whole
interface. When your teammate's detection backend is ready you only replace the
two functions marked "REPLACE ME" - nothing else changes.

HOW THE SCREEN WORKS (so the data below makes sense)
----------------------------------------------------
    +------------------------------------------------+
    |  Total Ships: 70     Dark Vessels: 50           |  <- only once a scene
    +------------------------------------------------+     is selected
    |                                  +-----------+  |
    |            THE MAP               | Ship      |  |  <- only once a vessel
    |                                  | details   |  |     is clicked
    |                                  +-----------+  |
    +------------------------------------------------+
    | (select) (SAR)                Dark Vessels [o] |  <- always there
    +------------------------------------------------+

  * The SELECT button (pencil) shows every scene as a big see-through blue
    box. Click one and it becomes the selected scene: dotted outline, the map
    flies to it, the bar on top appears and the vessels are drawn.
  * The SAR button (layers) lays the radar picture of the scene over the map.
  * The DARK VESSELS switch chooses WHICH vessels are on the map:
        on  -> the dark ones (no AIS)      <- turns itself on with the scene
        off -> the "safe" ones (with AIS)

HOW TO RUN IT
-------------
    pip install flask
    python app.py
    # then open http://127.0.0.1:5000 in your browser

FOLDER LAYOUT
-------------
    app.py                  <- you are here (the server)
    templates/index.html    <- the page structure (map, controls, cards)
    static/css/style.css    <- the base look: colours, sizes, layout
    static/css/ui.css       <- the polished components (controls, cards)
    static/js/baklava.js    <- all button logic and map drawing
    static/img/             <- the logo and the frames it is drawn from
    static/anim/            <- the loading animation
"""

import math
import random

from flask import Flask, jsonify, render_template

# ==============================================================================
#  SECTION 1 - SETTINGS YOU WILL PROBABLY WANT TO CHANGE
# ==============================================================================
# Everything in this block is sent to the web page when it loads, so you can
# change the branding and every word on screen here without touching the HTML.

APP_SETTINGS = {
    # --- Branding (top-left corner of the page) --------------------------
    "project_title": "BAKLAVA",
    "project_subtitle": "BAlKan Location Analisys of Vessels with AI",

    # Path to your logo, inside static/. It is shown as it is - no circle or
    # frame behind it - and scaled to fit --logo-size in static/css/style.css.
    # If the file is missing, nothing is drawn in its place.
    "logo_path": "img/logo.svg",

    # --- The loading animation --------------------------------------------
    # Shown in the middle of the screen while the server is being waited for.
    # The file is built from the eight drawings in static/img/frame_*.svg and
    # animates itself, so it works as a plain picture - swap in a .gif here and
    # it will behave the same way.
    "loading_animation_path": "anim/loading.svg",
    # The line written under it. Set it to "" for the animation on its own.
    "loading_text": "Working...",
    # How long the animation stays on screen AT LEAST, in milliseconds. Without
    # it a fast answer makes the animation flash for one frame and vanish,
    # which reads as a glitch rather than as progress.
    "loading_min_ms": 500,

    # --- The three controls under the map ---------------------------------
    # The first two are icon buttons, so these strings are their tooltips and
    # their screen-reader names. The third one is the switch on the right.
    "btn_select_title": "Select a SAR scene",
    "btn_sar_title": "Show the SAR overlay",
    "toggle_dark_label": "Dark Vessels",

    # --- The bar that appears on top once a scene is selected --------------
    "stat_total_label": "Total Ships:",
    "stat_dark_label": "Dark Vessels:",

    # --- The card that appears when a vessel is clicked --------------------
    "ship_details_title": "Ship details:",

    # --- Where the sidebar sections lead ----------------------------------
    # "AIS Database Archive" is a real site, so that entry is a plain link.
    "link_ais_archive": "https://supm.online/ais/",

    # --- Texts of the small notification strip ----------------------------
    # It floats over the top-left of the map and says one line at a time.
    "msg_scenes_failed":  "Could not load the scenes. Try again.",
    "msg_scene_failed":   "Could not load that scene. Try again.",
    "msg_pick_scene":     "Pick a scene on the map",
    "msg_pick_scene_off": "Scene selection cancelled",
    "msg_no_sar":         "This scene has no SAR overlay yet",
    "msg_sar_needs_scene": "Select a scene first",

    # --- Map defaults ------------------------------------------------------
    # Where the map sits before any scene is chosen (the whole Balkan region,
    # exactly the view in the mock-up).
    "map_default_lat": 42.0,
    "map_default_lon": 20.0,
    "map_default_zoom": 5,
}


# ==============================================================================
#  SECTION 2 - FAKE DATA (REPLACE ME once the real backend exists)
# ==============================================================================
#
# A SCENE is one SAR acquisition: the blue box you click on the map. It is
# described by its FOUR CORNERS, not by two edges, so a scene that was flown at
# an angle is drawn at that angle (the boxes really are tilted - see the
# mock-up). The corners must be given in order around the shape, either
# clockwise or anticlockwise; do not repeat the first one at the end.
#
#     "corners": [[lat, lon], [lat, lon], [lat, lon], [lat, lon]]
#
# Everything below is generated so the demo has believable numbers. Your real
# backend will simply return its own scenes and vessels in the same shape.

# The recipe for each demo scene: where it sits, how big it is and how far it
# is turned. SCENE_BLUEPRINTS is only used to MAKE the fake data - the frontend
# never sees it.
SCENE_BLUEPRINTS = [
    {
        "id": "TYR_20240311",
        "label": "Tyrrhenian Sea - 11 Mar",
        "lat": 41.05, "lon": 12.35,          # centre of the scene
        "width_deg": 2.0, "height_deg": 1.7,  # its size in degrees
        "rotation_deg": 28,                   # how far it is turned
        "total": 70, "dark": 50,              # how many vessels, of which dark
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

# Names used for the vessels that DO broadcast AIS, so the demo cards read
# like real ones instead of "Ship 14".
DEMO_SHIP_NAMES = [
    "MV Adriatic Star", "MV Kalypso", "MT Pontos", "MV Danubia",
    "MT Levant Trader", "MV Zephyros", "MV Ionian Pearl", "MT Bosphorus",
    "MV Sirena", "MT Aegean Dawn", "MV Thalassa", "MV Nordic Ember",
]

DEMO_SHIP_TYPES = ["Cargo", "Tanker", "Fishing", "Container", "Bulk carrier", "Tug"]


def _rotate(dx, dy, degrees):
    """
    Turns the point (dx, dy) around (0, 0) by "degrees".

    Used twice below: once for the four corners of a scene, once for the
    vessels inside it, so both end up tilted by the same amount.
    Returns the new (dx, dy).
    """
    angle = math.radians(degrees)
    return (dx * math.cos(angle) - dy * math.sin(angle),
            dx * math.sin(angle) + dy * math.cos(angle))


def _scene_corners(blueprint):
    """
    Works out the four corners of one demo scene from its centre, its size and
    how far it is turned. Returns [[lat, lon], ...] going round the shape.
    """
    half_w = blueprint["width_deg"] / 2
    half_h = blueprint["height_deg"] / 2

    # The four corners of an upright box, before turning it.
    upright = [(-half_w, -half_h), (half_w, -half_h),
               (half_w, half_h), (-half_w, half_h)]

    corners = []
    for dx, dy in upright:
        rx, ry = _rotate(dx, dy, blueprint["rotation_deg"])
        # One degree of longitude is shorter than one of latitude away from the
        # equator; dividing by cos(lat) keeps the box looking square on screen.
        corners.append([
            round(blueprint["lat"] + ry, 6),
            round(blueprint["lon"] + rx / math.cos(math.radians(blueprint["lat"])), 6),
        ])
    return corners


def _make_vessels(blueprint):
    """
    REPLACE ME (2 of 2) - the inside of it, anyway.

    Invents the vessels of one scene. This is the exact shape one vessel has to
    have; keep the key names and the frontend keeps working unchanged.

      id           : anything unique inside the scene (string)
      lat, lon     : where it is
      dark         : True  -> no AIS signal   (the alert-coloured dot)
                     False -> normal vessel   (the pale dot)
      name         : what the details card shows as the title
      mmsi         : the AIS identifier, or "" for a dark vessel
      type         : "Cargo", "Tanker", ...    (free text)
      length_m     : length in metres
      heading_deg  : where it points, 0 = north
      speed_kn     : speed in knots
      detected_at  : when the radar saw it (free text, shown as it is)
      confidence   : how sure the detector is, 0..1
    """
    # A fixed seed means the same scene always produces the same vessels, so
    # the demo does not reshuffle itself every time you press reload.
    rng = random.Random(blueprint["id"])

    half_w = blueprint["width_deg"] / 2
    half_h = blueprint["height_deg"] / 2
    lat_scale = math.cos(math.radians(blueprint["lat"]))

    vessels = []
    for index in range(blueprint["total"]):
        is_dark = index < blueprint["dark"]

        # A random spot inside the upright box, then turned with the box so it
        # lands inside the tilted footprint too. The 0.88 keeps the vessels a
        # little away from the very edge.
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
    """
    REPLACE ME (1 of 2).

    The scenes drawn as blue boxes when the SELECT button is pressed.
    Response shape - one entry per scene:

      {"id": "...", "label": "...",
       "corners": [[lat, lon] x 4],
       "has_sar": True/False}

    "has_sar" only decides whether the SAR button has anything to show for that
    scene; the picture itself comes with the single scene, below.
    """
    scenes = []
    for blueprint in SCENE_BLUEPRINTS:
        scenes.append({
            "id": blueprint["id"],
            "label": blueprint["label"],
            "corners": _scene_corners(blueprint),
            "has_sar": False,        # no demo radar pictures shipped - see below
        })
    return scenes


def build_scene_detail(scene_id):
    """
    REPLACE ME (2 of 2).

    Everything about ONE scene: its shape, its vessels and its radar picture.

      id, label, corners : the same as in the list above
      totals             : {"total": .., "dark": ..} - the bar on top of the map
      vessels            : the list described in _make_vessels()
      sar_overlay        : the radar picture laid over the map, or None.
                           {"url": "/static/sar/TYR_20240311.png",
                            "corners": [[lat, lon] x 4]}
                           The corners say where the four corners of the picture
                           belong on the map; use the scene's own corners unless
                           the picture covers something else.

    NOTE ON THE MISSING PICTURES
    ----------------------------
    No demo radar images ship with this prototype, so sar_overlay is None and
    the SAR button falls back to a grey radar-like basemap over the scene - the
    button stays demonstrable. Drop a .png into static/sar/, fill sar_overlay in
    here, and the real picture is used instead without any frontend change.
    """
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


# ==============================================================================
#  SECTION 3 - THE SERVER ITSELF (you rarely need to touch this)
# ==============================================================================

app = Flask(__name__)


@app.route("/")
def home():
    """
    Serves the web page.

    render_template() takes templates/index.html, injects the values from
    APP_SETTINGS into it (that is what the {{ ... }} placeholders in the HTML
    are for) and sends the finished page to the browser.
    """
    return render_template("index.html", settings=APP_SETTINGS)


@app.route("/api/scenes")
def api_scenes():
    """
    The blue boxes. Called once, when the SELECT button is first pressed.
    Response:  {"scenes": [ ... see build_scene_list() ... ]}
    """
    return jsonify({"scenes": build_scene_list()})


@app.route("/api/scenes/<scene_id>")
def api_scene(scene_id):
    """
    One scene with its vessels. Called when a blue box is clicked.

    <scene_id> in the URL is whatever the user clicked, e.g.
    /api/scenes/TYR_20240311 . Flask passes it in as the scene_id argument.

    Answers 404 with a small JSON body if the id is unknown, so the page can
    report it instead of choking on an HTML error page.
    """
    scene = build_scene_detail(scene_id)
    if scene is None:
        return jsonify({"error": "Unknown scene"}), 404
    return jsonify(scene)


if __name__ == "__main__":
    # debug=True makes the server restart automatically every time you save a
    # file, and shows full error messages in the browser. Set it to False
    # before showing the app to anyone outside your team.
    app.run(debug=True, port=5000)
