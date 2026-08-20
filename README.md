# BAKLAVA pipeline walkthrough

A presentation copy of the app. It steps through the 11 acts of the SAR pipeline
one at a time, on a real scene, with the real per-stage timings from the run.

`baklava_app/` is untouched. This is a separate Flask app on port 5001 and it
talks to the same backend on 5080.

## Running it

    cd baklava_demo && python3 app.py        # http://127.0.0.1:5001

The .NET backend must be up first, and it needs the Development environment or
it will refuse to start:

    cd baklava_app/backend && dotnet run

## Driving it

Pick a scene, press Start, then:

| key | does |
|---|---|
| space, right arrow, page down | next step |
| left arrow, page up | previous step |
| Home / End | first / last step |

Clicking any chip in the bottom rail jumps straight to that step. Autoplay
advances every 6 s and fills the button as it counts down, so you can see it is
running. Everything can be driven from a presenter remote that sends
page up / page down.

Tick "Reprocess on the Jetson first" to trigger a real run before stepping
through, so the timings are from that moment rather than the last stored run.
It is slow, and it needs the Jetson reachable.

## What is real and what is not

Real, pulled live from the backend every time: the stage names, every
millisecond, the scene footprint and swath geometry, the geocoded overview
image, and all vessel positions and dark/known classification.

One step image is bundled rather than fetched, because the backend does not
serve it: the raw radar-geometry band, shown as an inset on step 1. It lives in
`static/img/stages/` and is keyed by scene id in
`APP_SETTINGS["demo_scene_assets"]`. Only the Bosphorus scene has one; any other
scene simply skips the inset and the rest of the walkthrough still works. It
cannot be derived from the API — it needs the original `.SAFE`.

The land mask on step 7 is **not** an image. It is drawn from real coastline
vectors, so it works for every scene. `data/coastline.json` holds GSHHS
full-resolution land and lake polygons for lon -14..46, lat 24..54, each with a
precomputed bounding box. `/api/coastline?w=&s=&e=&n=` filters it server-side so
the browser only ever receives the polygons for the scene on screen — tens of
features instead of nine thousand. The polygons are then clipped to the swath
quad in the browser, so the red never spills outside the ground the scene
actually covers.

If a scene ever shows no land mask, check its footprint against that bbox first.
That was the original bug: the coastline only covered the Balkans, so scenes off
North Africa came back empty.

Not real: the pacing. The detector finishes in about 1.7 s and cannot be paused
between stages without modifying and rebuilding the C++. The walkthrough is a
presentation layer over a real run, not a live trace.

The prose, the highlighted facts and the grouping of 26 raw stages into 11 acts
live in `APP_SETTINGS["demo_steps"]` in `app.py`. Edit them there.

## The parallel steps

Four steps are marked "runs in parallel" and do not advance the elapsed clock,
because their work is genuinely hidden behind another stage: the output raster
alloc, the terrain model, the engine load and the land mask. Summed serially the
stages come to about 3.0 s; the run takes about 1.86 s. That gap is the point,
and it is worth saying out loud.

## Geometry

The SAR image and the land mask are drawn by `SarQuadOverlay`, ported from the
production app. It corner-pins the picture with a `matrix3d` transform and clips
it to the swath polygon, so the black no-data corners never reach the map.

Vessels are drawn as their true oriented boxes from `vessel.corners`, not as
dots, and fall back to a circle if a scene ever returns a vessel without them.

The map fits once when the scene loads and does not move again while you step.
That is deliberate: it keeps the audience oriented, and it avoids a stale warp
transform on the corner-pinned image.
