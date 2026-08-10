# BAKLAVA web app

Prototype interface for the BAlKan Location Analisys of Vessels with AI project.

## Run it

```bash
pip install flask
python app.py
```

Then open <http://127.0.0.1:5000>.

## The screen

A header (logo + name on the left, hamburger on the right) and, under it, the
map — that is the whole interface. The map board has three parts:

```
+--------------------------------------------------+
|  Total Ships: 70      Dark Vessels: 50        ×  |  <- only with a scene open
+--------------------------------------------------+
|                                    +----------+  |
|                THE MAP             | Ship     |  |  <- only after a vessel
|                                    | details  |  |     is clicked
|                                    +----------+  |
+--------------------------------------------------+
| (pencil) (layers)            Dark Vessels  [ o ] |  <- always there
+--------------------------------------------------+
```

## What happens when you click

1. **The pencil** → every SAR scene appears as a big see-through blue box, and
   the map zooms out until all of them fit. The name of a scene follows the
   mouse over its box. Pressing the pencil again takes the boxes away.
2. **A blue box** → that scene is selected:
   * the other boxes go and this one keeps a **dotted outline**,
   * the map moves to it,
   * the bar on top appears with **Total Ships** and **Dark Vessels**,
   * the **Dark Vessels switch turns itself ON**, so the dark vessels are the
     first thing you see.
3. **The switch** → OFF draws **every** vessel of the scene: the dark ones (red
   dots, no AIS) together with the “safe” ones (white dots with a dark ring).
   ON leaves **only the dark ones** on the map. It turns itself on with the
   scene, so you start on the dark vessels and switch off to see everything.
4. **A vessel** → the **Ship details** card opens in the top-right corner of the
   map: status, coordinates, MMSI, type, length, heading, speed, when it was
   detected and how sure the detector is. The dot you opened wears a thicker
   ring. Clicking the sea, the ×, or pressing Escape closes the card.
5. **The layers button** → lays the SAR picture of the open scene over the map.
   No demo radar images ship with the prototype, so it currently falls back to a
   grey radar-style basemap limited to the scene and says so — see
   *The SAR overlay* below.
6. **The × in the top bar** → closes the scene and goes straight back to picking
   one.

The layers button and the switch both work *on* a scene, so until one is
selected they are greyed out and cannot be clicked, tabbed to, or reached at
all. Opening a scene brings them to life; closing it puts them back to sleep.
The pencil is never disabled — picking a scene is the one thing always on offer.
7. **Hamburger** → sidebar slides in from the right: *SAR Scenes* (where we are),
   *AIS Database Archive* (a link to <https://supm.online/ais/>, set in
   `APP_SETTINGS`), *Dark Vessel Alerts* (a popup for now).

Nothing on the page moves when something appears: the notification and the ship
card float over the map rather than sitting in the flow, and the map never
changes size except when the top bar comes and goes.

## Colours

| Colour | Used for |
|---|---|
| `#FFD29D` | the two round control buttons |
| `#918450` | the bars above and below the map |
| `#1F271B` | the switch when it is on, the dotted scene outline, dark surfaces |
| `#2F6FD0` | the see-through scene boxes |
| `#D0342C` | dark vessels |

## Where to change what

| I want to change… | File | Where |
|---|---|---|
| Title, subtitle, logo path | `app.py` | `APP_SETTINGS` |
| The label next to the switch | `app.py` | `toggle_dark_label` |
| The wording of the top bar | `app.py` | `stat_total_label`, `stat_dark_label` |
| The title of the ship card | `app.py` | `ship_details_title` |
| The notification wordings | `app.py` | the `msg_*` entries |
| The AIS Archive address | `app.py` | `link_ais_archive` |
| Where the map starts | `app.py` | `map_default_lat/lon/zoom` |
| The scenes | `app.py` | `SCENE_BLUEPRINTS`, `build_scene_list()` |
| The vessels and the radar picture | `app.py` | `build_scene_detail()`, `_make_vessels()` |
| Colours, fonts, sizes | `static/css/style.css` | the `:root` block at the top |
| The controls, the top bar, the ship card | `static/css/ui.css` | sections A–D |
| Map style (satellite, dark, grey) | `static/js/baklava.js` | `initMap()` |
| What the controls do | `static/js/baklava.js` | PART 8 |

The logo is `static/img/logo.svg` (set by `logo_path` in `APP_SETTINGS`). It is
drawn on its own — no circle or frame behind it — and scaled to `--logo-size`
in `static/css/style.css`. If the file is missing, nothing is shown in its place.

If you re-export the logo from Inkscape, crop the page to the drawing
(*File → Document Properties → Resize page to drawing*) before saving. An SVG
saved as a full A4 page is mostly empty space, and the mark then renders tiny
inside the header. `logo.svg` was cropped this way with a `viewBox` of
`64 105 79 130` — the artwork itself was not touched.

## The SAR overlay

`build_scene_detail()` in `app.py` returns `sar_overlay` for each scene:

```json
{ "url": "/static/sar/TYR_20240311.png",
  "corners": [[lat, lon], [lat, lon], [lat, lon], [lat, lon]] }
```

The frontend pins that picture to those corners on the map. **No demo images
ship with the prototype**, so `sar_overlay` is `None` and the layers button
falls back to a grey radar-style basemap clipped to the scene, with a note
saying the scene has no overlay yet. Drop a `.png` into `static/sar/`, fill
`sar_overlay` in, and the real picture is used instead — no frontend change.

## The loading animation

While the page waits for the server, a dimmed sheet covers it with the BAKLAVA
mark being rubbed out and drawn back in on a loop. It shows up when the scene
list is fetched and when a scene is opened.

`static/anim/loading.svg` is one picture that animates itself — no JavaScript,
no frame files to load. It is **assembled, not drawn**: change one of the eight
drawings in `static/img/frame_*.svg` and rebuild it with

```
python static/anim/build_loading_animation.py
```

which is also where the speed lives (`FRAME_MS`). Its size on screen is
`--loader-size` in `static/css/style.css`.

Because a local server answers in a few milliseconds, `loading_min_ms` in
`app.py` keeps the animation on screen for at least 500 ms — otherwise it would
flash for a single frame and read as a glitch. Set it to `0` to switch that off.

## Testing by hand

Open the page, press F12 → Console:

```js
BAKLAVA.startPicking()                 // as if the pencil was clicked
BAKLAVA.selectScene("TYR_20240311")    // open a scene straight away
BAKLAVA.setDarkToggle(false)           // show every vessel of the scene
BAKLAVA.setSar(true)                   // the SAR overlay
BAKLAVA.closeScene()                   // back to picking
BAKLAVA.notify("Anything", "error")    // "success", "error", "info", "pending"
BAKLAVA.focusOn(43.2, 28.6, 8)
BAKLAVA.scene                          // the scene currently open
```

## Backend contract

Your teammate's service needs to produce these two responses.

`GET /api/scenes` — the blue boxes. `corners` are the four corners of the SAR
footprint, in order around the shape, so a scene flown at an angle is drawn at
that angle.

```json
{ "scenes": [
    { "id": "TYR_20240311",
      "label": "Tyrrhenian Sea - 11 Mar",
      "corners": [[39.9, 11.4], [41.0, 13.0], [42.2, 12.3], [41.1, 10.7]],
      "has_sar": false }
] }
```

`GET /api/scenes/<id>` — that scene with its vessels.

```json
{
  "id": "TYR_20240311",
  "label": "Tyrrhenian Sea - 11 Mar",
  "corners": [[39.9, 11.4], [41.0, 13.0], [42.2, 12.3], [41.1, 10.7]],
  "totals": { "total": 70, "dark": 50 },
  "vessels": [
    { "id": "TYR_20240311-001",
      "lat": 41.05, "lon": 12.35,
      "dark": true,
      "name": "Dark contact 01",
      "mmsi": "",
      "type": "Unknown",
      "length_m": 120,
      "heading_deg": 268,
      "speed_kn": 8.4,
      "detected_at": "2024-03-11 04:17 UTC",
      "confidence": 0.87 }
  ],
  "sar_overlay": null
}
```

`dark: true` means no AIS signal — that vessel is drawn in the alert colour and
is the only kind left on the map when the switch is on. Every field of a vessel
except `lat`, `lon` and `dark` is optional: a missing one simply leaves its row
off the details card.

Keep those key names and nothing in the frontend has to change.

## Notes

- Leaflet and the fonts load from the internet. To work fully offline, download `leaflet.css`, `leaflet.js` and the font files into `static/` and update the links in `templates/index.html`.
- Set `debug=True` to `False` in `app.py` before showing the app outside your team.
