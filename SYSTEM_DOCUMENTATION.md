# BAKLAVA — System Documentation

What this is, how it's laid out, and what every file does. Covers the whole
working system: the local dev app (`baklava_app/`) and the two pieces that
run on the Jetson (`forsee/jetson/`, `forsee/yolov8s-obb-cpp/`), which are
mirrored locally in this repo so they can be reviewed and version-controlled
without needing a live SSH session to the device.

## Architecture, in one paragraph

A Sentinel-1 SAR scene (`.SAFE` product or already-geocoded `.tif`) is picked
in the browser. The browser talks to a local **Flask** app, which proxies to
a local **ASP.NET Core** backend, which shells out to a bash script
(`jetson_client.sh`) that talks HTTP to a **Flask listener** running on the
Jetson. The listener runs the actual **C++/CUDA/TensorRT detector** in a
Docker container, gets back a JSON report plus an overview image and crop
JPEGs, reprojects the overview to Web Mercator with **GDAL**, and hands
everything back up the chain. The browser draws the ships as rotated boxes
on a Leaflet map, with the reprojected SAR image as an optional layer under
them.

```
Browser (Leaflet)
   |  fetch /api/scenes/...
   v
Flask (app.py)                    <- local dev server, port 5000
   |  urllib proxy
   v
ASP.NET Core (backend/)           <- local dev server, port 5080
   |  subprocess
   v
jetson_client.sh                  <- thin curl wrapper
   |  HTTP + Bearer token
   v
listener_service.py (Jetson)      <- Docker container, port 8080
   |  subprocess -> docker exec/run
   v
sar_ship_detect (C++/CUDA)        <- Docker container, GPU
```

## Directory map

```
baklava_app/                      local dev app (this is what you run)
  app.py                          Flask: serves the page, proxies /api/*
  jetson_client.sh                bash: HTTP client for the Jetson listener
  templates/index.html            page markup
  static/js/baklava.js            all client-side logic
  static/css/{style.css,ui.css}   layout / component styles
  backend/                        ASP.NET Core Web API
    Program.cs                    startup, DI, middleware
    Controllers/                  HTTP endpoints
    Services/                     Jetson client, scene cache, AIS matcher
    Common/                       DTOs, JSON normalization, projection math
    dark-vessel-detection/        separate solution: the AIS matching API

forsee/jetson/                    mirrors /data/code/Baklava/jetson on the Jetson
  listener_service.py             the HTTP daemon (runs in Docker)
  warp_overview.py                UTM -> Web Mercator reprojection
  txt_to_json.py                  legacy-backend report converter
  Dockerfile                      listener's container image
  docker-compose.yml              how the listener container is started

forsee/yolov8s-obb-cpp/           mirrors /data/code/Baklava/yolov8s-obb-cpp
  run.sh                          host wrapper: builds the detector's args
  docker-compose.yml              persistent detector container (see caveat below)
  pipeline/                       the C++/CUDA source
  data/                           model weights, DEM, coastlines, input/output
  MANUAL.md                       upstream project's own reference doc
```

---

## 1. `baklava_app/` — the local dev app

### `app.py`

Flask app. Doesn't talk to the Jetson directly — every `/api/*` route is a
thin proxy to the ASP.NET backend at `BAKLAVA_BACKEND_URL`
(`http://localhost:5080` by default).

- `_backend_open(path, range_header, method, timeout)` — builds the
  `urllib.request.Request`. Passes `data=b""` on POST so the request carries
  `Content-Length: 0`, which ASP.NET expects on a body-less POST.
- `_proxy_json(path, method, timeout)` / `_proxy_binary(path, mimetype)` —
  forward the response verbatim (status, body, and for binary responses
  `Content-Range`/`Accept-Ranges`/`Content-Length` too, so JPEG range
  requests work).
- `BACKEND_TIMEOUT = 150` for ordinary calls; `PROCESS_TIMEOUT = 1800` for
  `/process`, since detection (especially a raw `.SAFE`) is a
  minutes-capable job, not a request.
- Routes: `/` (renders `index.html`), `/api/scenes`, `/api/scenes/available`
  (POST), `/api/scenes/<id>` , `/api/scenes/<id>/process` (POST),
  `/api/scenes/<id>/overview`, `/api/scenes/<id>/crops`,
  `/api/scenes/<id>/crops/<path>`.
- `APP_SETTINGS` — every user-facing string and default (labels, messages,
  default map centre/zoom). Passed into the Jinja template as `settings`.

### `jetson_client.sh`

Bash CLI wrapping the Jetson listener's HTTP API with `curl`. The backend
never talks to the Jetson directly — it always shells out to this script
(via `JetsonClientService`), which keeps the auth token and the actual
network calls in one auditable place. Sub-commands: `list`, `get NAME`,
`get-all`, `list-images`, `process NAME [--backend cpp]`, `overview NAME`,
`crops NAME [--thumbs|--full|--all] [--only-detections] [--tar]`,
`crops-manifest NAME`, `send-coords FILE [name]`.

`crops` without `--tar` fetches the manifest, then for each wanted file
compares local size + CRC32 against the manifest before deciding whether to
fetch — so a repeated call, or one that was interrupted mid-download, only
transfers what's actually missing or corrupt. `--tar` instead hits
`GET /scenes/<name>/crops.tar` and extracts the response in one shot — this
is what the .NET backend uses now, because fetching 310 thumbnails as 310
separate `curl` processes cost ~14s of pure fork/exec overhead for 0.5 MB of
actual data.

### `templates/index.html`

The whole UI shell in one file: top bar, map container, stats bar, ship
detail card, scene picker panel, processing-time panel, sidebar, popup,
loader overlay. All content is templated from `APP_SETTINGS` so copy lives
in one place (`app.py`), not scattered through markup.

### `static/js/baklava.js`

All client logic. No build step, no framework — one file, plain DOM and the
Leaflet API. Organized as:

- **Layers**: `scenePickerLayer` (candidate scene boxes), `sceneOutlineLayer`
  (selected scene's dashed border), `vesselBoxLayer` (rotated ship boxes),
  `vesselDotLayer` (circle markers, used at low zoom / when there's no box
  geometry), `sarOverlayLayer` (the reprojected radar image).
- `initMap()` — creates the Leaflet map, the OSM base tile layer, and every
  overlay `LayerGroup`; wires `click` (closes the ship card) and `zoomend`
  (`updateVesselDotVisibility`).
- **Scene list / picker**: `loadSceneList({refresh})` (cached fetch of
  processed scenes), `loadAvailableImages()` (POSTs `/available`, returns raw
  Jetson images not yet processed), `startScenePicking()` /
  `stopScenePicking()` / `renderPicker(scenes, images)` — the picker groups
  "Ready to process" (raw Jetson images, tagged SAFE/TIF) above "On the map"
  (already-processed scenes).
- **Naming**: `sentinelParts(name)` parses the Sentinel-1 filename convention
  (`S1[A-D]_IW_GRDH_..._yyyyMMddThhmmss_...`) into mission/mode/product/date;
  `regionName(lat,lon)` maps a centroid into one of ~13 hard-coded sea-region
  bounding boxes (Black Sea, Aegean, Adriatic, ...); `sceneDisplayName` /
  `imageDisplayName` turn the raw product ID into something a non-technical
  user can read, e.g. "Black Sea — 7 Aug 2026, 04:13 UTC" instead of
  `S1C_IW_GRDH_..._1BB9`.
- **Processing**: `processImage(imageName)` — POSTs `/process`, times the
  round trip with `performance.now()` (stored as `lastClientMs`, shown in
  the timings panel as "browser fetch"), on success reloads the scene list
  and opens the result; on failure shows the Jetson's own error text in a
  popup.
- **SAR overlay** — `squareToQuad(points)` computes a projective (not just
  affine) 2D homography mapping the unit square to four arbitrary screen
  points, used because the overview's four corners are a general
  quadrilateral in screen space once Leaflet projects them. `SarQuadOverlay`
  (a custom `L.Layer`) creates an `<img>`, positions it with a CSS
  `matrix3d()` built from that homography, and re-warps on every
  `viewreset/zoom/move` event. `swathClipPath(bounds, swath)` builds a CSS
  `clip-path: polygon(...)` from the true radar swath quad (read from the
  `.SAFE` manifest server-side) converted into the overview's own
  0–100% coordinate space via `mercatorY(lat)`, so the image doesn't show the
  black wedges outside the actual beam. `.sar-quad`'s CSS also feathers the
  outer ~4% of every edge with a `mask-image` gradient so the layer blends
  into the base map instead of ending in a hard rectangle.
- **Ships**: `drawVessels()` draws either an `L.polygon` (if the vessel has
  `corners`, i.e. real oriented-box geometry from the detector) or an
  `L.circleMarker` fallback, for every vessel in the current scene (filtered
  to dark-only if that switch is on). `updateVesselDotVisibility()` hides the
  circle markers once boxes are available and either the SAR overlay is on or
  zoom ≥ `DOT_HIDE_FROM_ZOOM` (12) — so the map isn't dot-cluttered once boxes
  are legible, but low-zoom / no-geometry scenes still show something.
  `highlightVesselDot`/`vesselDotStyle`/`vesselBoxStyle` handle the
  open/selected visual state.
- **Timings panel**: `renderTimings()` reads `selectedScene.timings` (an
  array of `{stage, ms}` from the Jetson) plus `lastClientMs`, renders one
  row per stage with a proportional bar (`formatMs` switches to seconds
  above 1000ms). `setTimingsVisible(visible)` toggles the panel and the
  stopwatch button's active state.
- **Chrome**: `notify()`/`hideNotice()` (toast messages), `showPopup`/
  `hidePopup` (modal, used for processing failures — message text is
  `white-space: pre-line` so multi-line Jetson stderr reads correctly),
  `showLoader`/`hideLoader`/`runWithLoader(job, message)` (the full-screen
  spinner; enforces `loading_min_ms` so fast calls don't flash), sidebar
  open/close.
- `window.BAKLAVA` — everything above exposed on `window`, purely so it can
  be driven from a browser console/devtools during manual testing.

### `static/css/style.css` / `static/css/ui.css`

`style.css` is layout and page chrome (topbar, map board, sidebar, popup).
`ui.css` is components (stats bar, control buttons, switches, ship card,
scene picker, timings panel, notices, loader). Both are plain CSS with
custom properties (`--color-*`, `--radius-*`, `--speed-*`) defined once at
the top of `style.css` and reused everywhere — no preprocessor.

### `backend/` — ASP.NET Core Web API

**`Program.cs`** — minimal-hosting startup: registers controllers, CORS
(allowing the Flask origin), `JetsonClientService`, `SceneCatalogService`,
and `DarkVesselMatchService` (as a typed `HttpClient`), then
`app.MapControllers()`.

**`Controllers/ScenesController.cs`** — the whole scene API surface:

| Route | What it does |
|---|---|
| `GET /api/scenes` | lists processed scenes with footprint + label |
| `GET /api/scenes/{name}` | full detail: vessels, SAR overlay, crop summary, timings |
| `GET /api/scenes/{name}/raw` | the normalized-but-unprojected JSON, for debugging |
| `GET /api/scenes/{name}/overview` | proxies the JPEG (range-enabled) |
| `GET /api/scenes/{name}/crops` | crop manifest (counts/sizes only — see `EnsureCropsManifestAsync`) |
| `GET /api/scenes/{name}/crops/{path}` | one crop/thumbnail JPEG |
| `POST /api/scenes/sync` | pulls every available report from the Jetson at once |
| `POST /api/scenes/available` | raw Jetson images not yet processed |
| `POST /api/scenes/{name}/process` | runs detection |

`ComputeDarkIdsAsync` calls `DarkVesselMatchService.FilterKeepAsync` per
scene and folds the result into each `VesselDto.Dark`; if the matcher is
disabled or throws, every detection is reported dark rather than the
request failing.

**`Services/JetsonClientService.cs`** — wraps `jetson_client.sh` via
`Process`. `RunAsync(args, timeout, stdin, ct)` is the general case
(120s default); `RunProcessAsync(args, ct)` is for detection specifically
(1800s). Passes `BAKLAVA_DEST_DIR`/`BAKLAVA_TOKEN`/`BAKLAVA_BACKEND` through
the child process's environment rather than as CLI args, so the token never
shows up in `ps`.

**`Services/SceneCatalogService.cs`** — the local disk cache in front of the
Jetson. `GetSceneAsync` fetches+normalizes+caches a report; `EnsureOverviewAsync`
/`EnsureCropsManifestAsync` fetch only if the local copy is missing;
`EnsureCropsAsync(name, full)` fetches the crop *files* (via `--tar`),
skipping the network entirely if a manifest and at least one JPEG are
already on disk; `Forget(name)` invalidates the cache after reprocessing.

**`Services/DarkVesselMatchService.cs`** — AIS cross-reference, run in-process
against `DarkVessel.Core`'s `Matcher` and the MongoDB archive (`MongoAisSource`).
`Enabled` is false when no archive is registered (`Mongo:ConnectionString` blank),
in which case every detection is reported dark rather than the app throwing.
`FilterKeepAsync(ships, timestampUtc, ct)` fans out one `MatchStatusAsync` call
per detection (bounded by `DarkVessel:MaxConcurrentMatches`, default 8) and
returns the set of detection IDs that should still count as dark.

**`Common/SceneNormalizer.cs`** — converts whichever raw JSON shape the
Jetson returned (legacy Python backend vs. the C++ `detections` array) into
one internal shape (`{meta, ships[]}`). For the C++ shape it also
recovers a `col_px`/`row_px` centroid from each detection's `corners_pixel`
(used for its own sanity checks) and passes the oriented-box
`corners_lonlat` straight through as `ship.corners`.

**`Common/SceneProjector.cs`** — pure functions turning normalized JSON into
the DTOs the frontend consumes: `Footprint` (scene's 4 corners — prefers the
detector's own `footprint_lonlat`, falls back to a bounding box over ship
positions), `OverviewBounds`/`Swath` (read `overview_bounds_lonlat` /
`swath_lonlat`, both written by `warp_overview.py`), `Timings` (parses the
`timings` array into `TimingStage` records), `Label` (mission code + parsed
timestamp), `Vessels` (per-ship `VesselDto`, including the `Corners`
oriented-box outline when present).

**`Common/Validation.cs`** — `IsSafeName(name)`: `^[A-Za-z0-9_.\-]+$`, the
same allowlist the Jetson listener enforces, checked again here so a
malformed scene id never even reaches `jetson_client.sh`.

**`Common/ConfigExtensions.cs`** — `config.Require(key)` /
`RequireAll(keys)`: throws a clear startup error naming exactly which
`appsettings` key is missing, instead of a null-reference deep in
`JetsonClientService`'s constructor.

**`appsettings.json` / `appsettings.Development.json`** — the former ships
blank placeholders on purpose (`JetsonClient:ScriptPath`,
`JetsonClient:DestDir` and `Mongo:ConnectionString` are empty); the latter
(git-ignored) has the real values plus `JetsonClient:Token` and
`JetsonClient:Backend=cpp`. The Mongo connection string lives in user-secrets
(or `MONGODB_URI`), not in either file.

**`Controllers/CoordsController.cs`** — thin proxy to the Jetson's
`POST /coords`, unrelated to scene detection (used for a separate
coordinate-upload workflow).

### `backend/dark-vessel-detection/` — the AIS matcher (separate solution)

Its own `.sln`. The main backend now links two of its projects in-process
(`DarkVessel.Core` + `DarkVessel.Infrastructure`, via `ProjectReference`), so
matching needs no second service. `DarkVessel.Api` remains a standalone host
for the live AIS collector and its console.

- **`DarkVessel.Core`** — `Models.cs`/`WriteModels.cs` (DTOs), `IAisSource.cs`
  (abstraction over "where do AIS positions come from"), `IAisArchive.cs`
  (that plus the writes the collector needs),
  `InMemoryAisSource.cs` (test/dev implementation), `GeoUtils.cs`
  (haversine-type distance math), `Matcher.cs` (the actual
  detection-to-AIS-track matching logic — nearest track within a distance/time
  window counts as "seen on AIS", anything left over is dark).
- **`DarkVessel.Infrastructure`** — `MongoAisSource.cs`/`MongoAisOptions.cs`
  (the AIS archive as it is stored today: MongoDB Atlas, database `ships`,
  collection `ships_table` — the import of the old MySQL `ais_positions`
  table; this is the default, selected by `Ais:Source`), `HttpAisSource.cs`
  (the older client for the Baklava HTTP API) with `BaklavaApiOptions.cs`,
  `AisStore.cs` (direct MySQL via Dapper),
  `AisStreamModels.cs`/`AisStreamOptions.cs`/`AisStreamCollectorService.cs`
  (a background service consuming a live AIS stream), `CollectorState.cs`.
- **`DarkVessel.Api`** — `Program.cs` exposes the matcher and the collector as
  their own web API (`/api/match`, `/api/status`, `/api/control/start|stop`,
  `/api/archive-stats`) on port 5252. The backend no longer calls it; it is how
  you run and watch the live AIS collector.
- **`tests/DarkVessel.Core.Tests/MatcherTests.cs`** — unit tests for the
  matching logic.

---

## 2. `forsee/jetson/` — the Jetson-side HTTP daemon

Deployed as the `baklava-listener` Docker container (`docker compose up -d`
from this directory, on the Jetson). Owns nothing computational itself — it
validates requests, shells out to `run.sh` for the actual detector, and
serves the results back over HTTP.

### `listener_service.py`

Flask app, `network_mode: host`, port 8080.

**Path constants** — `BASE=/data/code/Baklava`, and everything else derived
from it: `OUTBOX` (finished JSON reports), `IMAGES_DIR` (raw scenes dropped
here to be processed), `PROCESSED_DIR` (`.done`/`.cpp.done` markers),
`CPP_WORK_DIR`/`CPP_INPUT_DIR` (the C++ detector's own output/input trees).

**Helpers**

- `log(msg)` — UTC-timestamped, written to both stdout (`docker logs`) and
  `listener_service.log` (survives a container restart).
- `is_processed(name)` — any `.done` marker present.
- `is_input(path)` — true for a file ending in `IMAGE_EXTS`
  (`.tif/.tiff/.jpg/.jpeg/.png`) or a directory ending in `DIR_EXTS`
  (`.safe`) — the single predicate that decides what shows up in `/images`
  and what `/process` will accept.
- `link_into(src, dest_dir, name)` — hard-links (not copies) a raw file into
  the detector's input directory; a GRDH scene is ~1-2 GB, so a copy would
  be pure wasted I/O on every call. Falls back to a symlink across
  filesystems. Idempotent.
- `check_auth()` (`before_request`) — Bearer-token gate; a no-op if
  `BAKLAVA_TOKEN` is unset.
- `safe_name(name)` — the same `^[A-Za-z0-9_.\-]+$` allowlist as the .NET
  side; the only thing standing between a scene name and path traversal.

**Overview reprojection** — `warp_overview(out_dir, stem, result_json,
scene_path)` calls into `warp_overview.py`'s `main()` **in-process** (an
`import`, not a subprocess/container spawn — see that file's own section for
why), times it, and returns the parsed `overview_bounds_lonlat` /
`swath_lonlat` / `warp_ms`. `parse_timings(*streams)` regex-matches the
detector's own `[time] stage   1234.5 ms` lines out of its
stdout+stderr so the frontend can show a per-stage breakdown instead of one
opaque total. `scene_output_dir(name)` strips the `__cpp` suffix a scene id
carries in the outbox and maps it back to the detector's actual output
folder.

**Routes**

| Route | Notes |
|---|---|
| `GET /list` | outbox filenames, newest first |
| `GET /scenes/<name>` | the report JSON |
| `GET /scenes/<name>/overview` | prefers `_overview3857.jpg`, falls back to the un-warped `_overview.jpg` |
| `GET /scenes/<name>/crops` | manifest only |
| `GET /scenes/<name>/crops.tar?tier=&only=` | every wanted crop file, one tar, built in memory from the manifest |
| `GET /scenes/<name>/crops/<path>` | one file; `os.path.realpath` + prefix check blocks `../` |
| `GET /scenes-all` | tar of every report at once |
| `POST /coords?name=` | atomic write (`.tmp` + `os.replace`) of an arbitrary JSON payload |
| `GET /images` | raw scenes not yet processed |
| `POST /images/<name>/process?backend=cpp\|legacy` | runs detection |

**`_process_legacy`** — Option A, the older Python/TensorRT backend.
Untouched this session; kept for scenes that still go through it.

**`_process_cpp`** — Option B, the one actually used. Marker check → decide
`scene_in` (a `.SAFE` directory is passed as-is, since `run.sh`'s per-call
bind mount is keyed on `dirname(scene)` and a symlinked directory would mount
the wrong thing; a flat file goes through `link_into`) → run `CPP_RUN_SH
detect scene_in -o out_dir` as a subprocess, timing it → on failure, return
the **last 8 lines of the detector's own stderr** (a missing DEM, an
unbuilt engine — actionable, versus a bare "detect failed") → parse timing
stages, add a synthetic "detector container startup" stage (wall time minus
the detector's own reported `TOTAL`) → call `warp_overview`, fold its
bounds/swath/timing into the payload → append "jetson handler total" →
write the report to `OUTBOX/<stem>__cpp.json` atomically → mark done.

### `warp_overview.py`

Standalone script, also imported as a module by the listener.

The detector's overview JPEG is a decimation of the L2 raster, which sits on
the scene's **UTM** grid (whichever zone the detector picked via
`AUTO:42001`). Placing that directly on a Leaflet map — Web Mercator — with
a lat/lon bounding box is wrong by 1.4–2.6 km, because no single affine or
even projective transform maps a UTM grid onto Web Mercator: lines of
constant northing curve in lat/lon, and Mercator's own latitude stretch is
nonlinear on top of that. So this reprojects the actual pixels:

- `utm_epsg(lon, lat)` — picks the UTM zone/hemisphere EPSG code for a point.
- `to_lonlat(epsg, xs, ys)` — batch-converts projected coordinates back to
  WGS84 lon/lat (used to report the final Mercator bounds in a form the
  frontend can consume directly).
- `swath_from_safe(scene_path)` — if given a `.SAFE` directory, regex-reads
  the `<gml:coordinates>` block out of `manifest.safe` — the true radar
  swath outline, narrower than the scene's own bounding rectangle since a
  GRDH scene is trimmed to a parallelogram. Returns `None` for a flat `.tif`
  input, which has no swath concept beyond its own rectangle.
- `main(overview, footprint_json, out_jpg, out_meta, scene_path=None)` —
  reads the detector's `footprint_lonlat` from its JSON report, converts the
  four corners to the scene's UTM zone, uses them to georeference the source
  JPEG via `gdal_translate -a_ullr` (a VRT, not a copy), `gdalwarp`s that to
  EPSG:3857 with `-multi -wo NUM_THREADS=ALL_CPUS` (roughly halves wall time
  on the Jetson's 12 cores versus single-threaded), re-encodes as JPEG, and
  writes the Mercator bounds plus (if available) the swath quad to
  `out_meta` as JSON.

Runs **in-process** inside the listener container rather than spawning a
separate GDAL container per call — the listener's own image now has GDAL
built in (see `Dockerfile`), which removed ~1.5–3s of container-start
overhead that a `docker run` per scene used to cost.

### `txt_to_json.py`

Converts the legacy Python/TensorRT backend's plain-text ship report into
the same JSON shape the C++ backend produces, so `SceneNormalizer` on the
.NET side only has to understand one format regardless of which backend ran.
Untouched this session.

### `Dockerfile`

```
FROM ghcr.io/osgeo/gdal:ubuntu-small-latest
RUN apt-get install docker.io python3-pip
pip install --break-system-packages -r requirements.txt
COPY listener_service.py txt_to_json.py warp_overview.py ./
CMD ["python3", "listener_service.py"]
```

Base image switched this session from a plain `python:3.11-slim` to the
official GDAL image, specifically so `warp_overview.py` can run in-process
(previously the listener had no GDAL, so warping meant spawning a whole
separate `ghcr.io/osgeo/gdal` container per scene through the Docker
socket). `docker.io` (the CLI, not a daemon) plus the mounted
`/var/run/docker.sock` (see `docker-compose.yml`) are still required, since
`run.sh` — invoked as a subprocess *inside* this container — itself needs
to launch the detector container on the host's Docker daemon
(docker-outside-of-docker).

### `docker-compose.yml`

One service, `baklava-listener`: `network_mode: host` (so it can be reached
on the Jetson's own port 8080 without publish-mapping), `restart:
unless-stopped`, `BAKLAVA_TOKEN` required from `.env`, and two volumes — the
whole `/data/code/Baklava` tree (identity-mounted, same path in and out) and
the Docker socket.

### `requirements.txt` / `.env`

`requirements.txt`: `Flask==3.0.3` (GDAL's Python bindings come from the
base image, not pip). `.env`: `BAKLAVA_TOKEN`, the bearer token every
listener request must present.

---

## 3. `forsee/yolov8s-obb-cpp/` — the GPU detector

The actual SAR-to-ships pipeline: calibration, terrain correction,
inference, land masking, crop/overview rendering. C++ with CUDA kernels,
built into a Docker image and run either one-shot (`docker run --rm`) or,
new this session, execed into a container kept warm by `docker compose`.

### `run.sh`

Host-side wrapper the listener's `_process_cpp` calls as a subprocess. Its
job is entirely argument-building — the actual work happens inside the
container it launches.

- `run_container(entry, ...)` — the original path: `docker run --rm
  --runtime nvidia -u $(id -u):$(id -g) --entrypoint "$entry" "${MOUNTS[@]}"
  "$IMAGE" "$@"`, `exec`'d so the shell process is replaced (correct exit
  code passthrough, no leftover parent process).
- `persistent_up()` — `docker ps -q -f name=^/$PERSIST_NAME$ -f
  status=running`; true if the long-lived detector container (see the
  compose file below) exists and is running.
- `run_detect(entry, ...)` — new this session. If `persistent_up`, `exec
  docker exec -u $(id -u):$(id -g) "$PERSIST_NAME" "$entry" "$@"`; otherwise
  falls through to `run_container` unchanged. This is the only place the
  persistent-container path is chosen — everything else in the script just
  decides *which paths* to pass, via `SCENE_IN_ARG`/`SCENE_OUT`/`MODEL_DIR`/
  `GSHHG_DIR`/`DEM_DIR`, set one way if `USE_PERSIST` (real host paths,
  since the persistent container identity-mounts the whole tree — see its
  compose file) and another way if not (the old `/scene_in`, `/scene_out`,
  `/data/model`, ... aliases backed by per-call bind mounts in `MOUNTS`).
- `detect SCENE -o OUTDIR [flags]` (the main subcommand) — resolves `scene`
  to an absolute path, parses `-o`/`--save-render`/`--save-l2`/`--no-crops`/
  `--no-overview` (everything else forwards straight to the detector
  binary as `extra[]`), decides `is_l1` from the extension (`.SAFE`/`.safe`/
  `.zip` vs. a flat raster), builds the DEM/geoid arguments only for the L1
  path (aborting with an actionable message if `data/dem/dem.tif` is
  missing), and finally calls `run_detect /app/sar_ship_detect` with the
  assembled argument list.
- `shell` — interactive `bash` inside the (non-persistent) container with
  the whole `data/` tree mounted, for manual debugging.
- Env overrides: `IMAGE`, `ONNX_NAME`, `ENGINE_NAME`, `DEM_NAME`,
  `GEOID_NAME`, `OVERVIEW_MAX` (default 8192 — longest side of the overview
  render), `BOX_THICKNESS` (default **0** — see `main.cpp` below), `POL`
  (`VH`/`VV`), `PERSIST_NAME` (default `sar-ship-detect`).

### `docker-compose.yml` (persistent detector) — **added this session, unverified**

```yaml
services:
  sar-ship-detect:
    image: sar-ship-detect:latest
    container_name: sar-ship-detect
    restart: unless-stopped
    runtime: nvidia
    ipc: host
    user: "2002:2002"
    entrypoint: ["sleep", "infinity"]
    environment: [NVIDIA_VISIBLE_DEVICES=all, NVIDIA_DRIVER_CAPABILITIES=all]
    volumes: ["/data/code/Baklava:/data/code/Baklava"]
```

The point: `docker run --rm` pays GPU-runtime setup and model-load cost on
*every single detect call* (measured at ~1.1–1.3s this session, on top of
the actual ~8s of detection work). Keeping one container alive on `sleep
infinity` and `docker exec`-ing into it removes that per-call cost entirely
— `run.sh`'s `run_detect` already knows to prefer it when present.

`user: "2002:2002"` matches the Jetson account's `uid:gid`. This exists
because of a real failure hit this session: the Tegra `nvidia-container-runtime`
hook wires up GPU device-node permissions for whichever user a container is
**created** as. `docker compose up -d` with no `user:` starts the container
as root; `docker exec -u 2002:2002` into it then can't open the GPU device
nodes (`NvRmMemInitNvmap failed: Permission denied` → CUDA init error 35).
Setting `user:` at creation time means the nvidia hook configures those
devices for the same uid `run.sh` execs in as.

**This fix was applied (`docker compose up -d --force-recreate` completed
successfully) but never re-verified** — the SSH session to the Jetson
dropped before the follow-up smoke test could run. Treat the persistent-container
path as *implemented but not yet confirmed working*; if `run_detect`
silently falls back to `run_container` every time, `persistent_up` is
returning false (check `docker ps` for the container) or the exec itself is
still failing on GPU permissions (check `docker logs`/re-run the `nvidia-smi
-L` smoke test from inside the container). If the uid fix turns out to be
insufficient, the fallback path (`docker run --rm`, unmodified) still works
exactly as it did before this change — `run.sh` never breaks if the compose
service isn't running.

### `pipeline/` — the C++/CUDA source

**`main.cpp`** — entry point and orchestration. Parses `Config` (see
`config.cpp`), opens the `Scene` (raw `.tif` or, via `l1proc.cpp`, a `.SAFE`
terrain-corrected on the fly), builds/loads the TensorRT `Engine`, tiles the
scene, runs inference batch by batch, decodes + NMS's detections, applies
the land mask, writes the JSON report, and — if requested — renders the
overview JPEG (`launch_overview_rgb`) and/or the full debug JPEG
(`launch_render_rgb`), each optionally with boxes burned in
(`launch_draw_boxes`). Prints every stage's wall time as `[time] stage
1234.5 ms`, which is what `listener_service.py`'s `parse_timings` consumes.
**This session's fix**: box burn-in was gated on `dSea && cfg.box_thickness
> 0` (previously just `dSea`, with the thickness itself floored at
`std::max(2, ...)`) — at `--box-thickness 0` (now `run.sh`'s default) no
boxes are drawn at all, since a 2px-minimum box at 4:1 decimation was
completely covering the ~5px ship underneath it in the overview.

**`config.hpp`/`config.cpp`** — `Config` struct + CLI parsing for every flag
(`--safe`/`--tif`, `--dem`/`--geoid`/`--dem-is-ellipsoidal`, `--onnx`/
`--engine`, `--imgsz`/`--batch`/`--overlap`/`--streams`, `--db-lo`/`--db-hi`,
`--conf`/`--nms-iou`/`--max-det`, `--gshhg`/`--buffer-m`/`--coarse-decim`/
`--no-skip-land-tiles`, `--out-json`/`--out-jpg`/`--out-overview`/
`--overview-max`/`--out-crops`/`--out-l2`, `--crop-*`, `--box-thickness`,
`--no-fp16`/`--workspace-mb`, `-v`/`--verbose`).

**`scene.hpp`/`scene.cpp`** — `Scene`: opens a GDAL dataset (or adopts an
in-memory raster produced by `l1proc`), holds the geotransform, and converts
between pixel coordinates and lon/lat (`pixelToLonLat`) or projected map
coordinates (`pixelToMap`). `readAll(fastPath)` — `fastPath` bypasses GDAL's
generic `RasterIO` for a direct libtiff read, ~2.5x faster on the scene
sizes involved. `pixelDirToBearing` turns a pixel-space direction vector
into a true compass heading (used for each detected ship's heading).

**`safe.hpp`/`safe.cpp`** — `SafeProduct::open(path, pol)` parses a `.SAFE`
product's three annotation XMLs: the main product annotation (orbit state
vectors, slant-range/ground-range polynomials, azimuth timing), the
calibration LUT, and the noise LUT — for the requested polarisation only.

**`l1proc.hpp`/`l1proc.cpp`** — `runL1ToL2(L1Config, Scene&)`: the raw L1
GRD → geocoded L2 pipeline. Builds a coarse geolocation lattice (every 32nd
pixel by default, `coarse_step`), CUDA-calibrates DN→σ⁰
(`launch_calibrate`), then CUDA range-Doppler-geocodes onto a UTM grid at
the requested `pixel_spacing` (default 10m) using that lattice plus
bilinear interpolation for everything in between — full-resolution
range-Doppler solving on every pixel is unnecessary since geometry varies
smoothly.

**`rdgeom.hpp`/`rdgeom.cpp`** — the range-Doppler geometry math on the CPU
side: `geodeticToEcef` (lat/lon/height → ECEF + surface normal, so terrain
height can be added along the correct axis), `OrbitTable::build`/`sample`
(pre-samples the satellite orbit into a uniform table so a per-pixel lookup
is O(1) rather than a fresh Lagrange interpolation each time),
`zeroDoppler` (solves for the imaging time — 3 fixed Newton-style
iterations, deliberately unbounded-loop-free so the equivalent CUDA kernel
doesn't diverge across a warp), `srgrGroundRange` (slant-range → ground-range
polynomial evaluation).

**`kernels.cu`/`kernels.cuh`** — every CUDA kernel, each exposed as a
`launch_*` C++ wrapper:

| Kernel | Purpose |
|---|---|
| `k_calibrate` | DN² minus interpolated thermal/azimuth noise, over interpolated calibration² → σ⁰ |
| `k_rd_geocode` | per-output-pixel: bilinear-sample the coarse lattice + DEM height, iterate zero-Doppler 3x, resample σ⁰ |
| `k_quantise` | σ⁰ → dB, clamp to `[db_lo, db_hi]`, scale to 8-bit; simultaneously builds a coarse "has-data" mask |
| `k_preprocess` | crops + normalizes tiles into the 3-channel float tensor TensorRT expects |
| `k_decode` | reads raw network output, drops detections too close to a tile seam (suppresses double-counting a ship split across tiles), atomically compacts survivors |
| `k_nms_mask` | rotated-IoU NMS: Sutherland-Hodgman polygon clipping for oriented-box intersection, block-tiled (64×64) with the "column" boxes cached in `__shared__` so they're read once and reused by all 64 rows, output as a bitmask CPU code then walks |
| `k_render_rgb` / `k_overview_rgb` | full-res / box-averaged-decimated grayscale→RGB |
| `k_draw_boxes` | 4 GPU threads per detection (one per box edge), Bresenham-style line walk with square dot-stamping for thickness |

Grid-stride loops with a fixed `dim3 grid(64, 256)` throughout, so kernel
launch parameters don't need recomputing per scene size; `__restrict__`
pointers everywhere for the compiler to assume non-aliasing;
`fmaf`/`__saturatef`/`__log10f` intrinsics; shift-instead-of-divide in
`k_quantise` when the decimation factor is a power of two.

**`landmask.hpp`/`landmask.cpp`** — `LandMask::build` rasterizes GSHHG
coastline shapefiles (`data/GSHHS_shp/f/GSHHS_f_L*.shp`, full resolution) at
a coarse decimation into a lookup grid; `isLand`/`tileFullyLand` answer
per-point / per-tile land queries used both to skip land tiles before
inference (fewer tiles → less GPU work) and to drop detections found on or
near land (`--buffer-m`, default 100m) after the fact.

**`crops.hpp`/`crops.cpp`** — the crop-window system.

- `CropConfig` — `size` (1024px default), `quality` (80), `thumb` (256, 0 to
  disable), `threads` (0 = auto: `hardware_concurrency() - 2`), `min_sea`,
  `coast_lo`/`coast` (whether to add coastal windows at all).
- `CoarseStats::build` — two summed-area tables (integral images), one for
  land fraction and one for data-presence, so any rectangle's land/sea
  fraction is a 4-lookup O(1) query regardless of its size.
- `planCrops(W, H, stats, cfg)` — lays a plain grid of `size`-px windows,
  then (if `cfg.coast`) for every grid cell with some-but-not-much land,
  searches a local neighbourhood for a nearby window that's closer to
  50/50 sea/land — coastal crops are deliberately centred on the shoreline,
  since that's both where port traffic clusters and where false positives
  are most likely.
- `encodeCrops(gray, W, H, wins, cfg, dir, stem)` — thread pool
  (`std::atomic<size_t>` work-stealing index, no locks) that copies each
  window, JPEG-encodes it (+ a thumbnail), and CRC32s the result.
- `writeCropManifest(...)` — the JSON manifest: scene metadata, per-crop
  `id`/`file`/`thumb`/position/`sea_frac`/`land_frac`/byte counts/CRC32s,
  and which detections fall in which crop. **The CRC32 is what makes
  `jetson_client.sh crops` (non-tar) resumable** — the client compares
  local size+CRC before deciding to re-fetch anything.

**`engine.hpp`/`engine.cpp`** — `Engine::loadOrBuild` loads a cached
TensorRT `.engine`, or builds+caches one from the ONNX model if the cache
file is missing (a one-time, 10–20 minute cost); `createContexts(n)` makes
`n` execution contexts for the requested stream count; `enqueue` runs one
batch asynchronously on a given stream.

**`render.hpp`/`render.cpp`**, **`jpeg.hpp`/`jpeg.cpp`** — thin C++ wrappers
the kernels/crop-encoder call into for JPEG encoding (`writeJpegGray`/
`writeJpegRGB`) and any CPU-side render bookkeeping.

**`detection.hpp`** — the two POD structs everything downstream is built
from: `Det` (raw oriented box: centre, size, angle, score — GPU-native
layout) and `GeoDet` (the georeferenced version: 4 corners' pixel + lon/lat,
centre lon/lat, physical length/width, heading, score — what actually goes
into the JSON report).

**`util.hpp`** — small shared helpers (the `Timer` class used for every
`[time] stage  N ms` line, etc.).

### `pipeline/docker/Dockerfile`

Two-stage build: compiles `sar_ship_detect` against
`nvcr.io/nvidia/pytorch:26.03-py3` (has CUDA/TensorRT/cuDNN headers and
libs), then copies just the binary into a slimmer runtime stage with
`libgdal34t64`. This is the image `run.sh`'s `$IMAGE` points at
(`sar-ship-detect:latest`); rebuild with `docker build -f
pipeline/docker/Dockerfile -t sar-ship-detect:latest pipeline/`.

### `data/`

- `model/` — `model_b16_640.onnx` (frozen: batch 16, 640×640 input) plus
  its cached `.engine` file(s).
- `GSHHS_shp/{c,l,i,h,f}/` — GSHHS coastline shapefiles at five resolutions
  (crude → full); the detector uses `f` (full).
- `dem/` — Copernicus GLO-90 tiles + `egm96.tif` geoid grid (needed only for
  raw `.SAFE` input; a `.tif` is assumed already terrain-corrected).
- `input/`, `output/` — the detector's own scratch space; per-scene output
  subfolders here are what `scene_output_dir()` on the listener side points
  back into.

### `MANUAL.md`

The upstream project's own reference documentation — much more exhaustive
than this file on the pipeline internals (DEM preparation, full flag
reference, a documented timing run). Treat it as authoritative for anything
this doc doesn't cover.

---

## Known gaps / things to check before relying on this

1. **The AIS archive is a fixed snapshot.** It covers 31 Jul – 7 Aug 2026
   (1,048,575 positions in `ships.ships_table`), with 6 Aug missing entirely.
   A scene acquired outside that window matches nothing and comes back
   `UnknownNoCoverage`, which the UI paints the same as `Dark` — so check the
   scene's date before reading a 100%-dark result as a finding. Running the
   collector (`DarkVessel.Api`, START) is what extends the archive.
2. **The persistent detector container's GPU-permission fix is unverified**
   (see the `docker-compose.yml` section above) — confirm with `docker exec
   sar-ship-detect nvidia-smi -L` before assuming `run_detect` is actually
   using the fast path.
3. **SAR-overlay/box alignment**: georeferencing is exact (verified to 0px
   median error against the detector's own pixel coordinates once
   reprojected to EPSG:3857) — if boxes ever look offset from the imagery
   again, suspect the *display* transform first, not the detector.
