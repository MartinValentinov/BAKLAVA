# SAR Ship Detection — User Manual

What this system does: takes a Sentinel-1 GRD scene (float32 sigma0, VH,
terrain-corrected) and produces a list of oriented ship detections with WGS84
coordinates, heading, and size — plus an optional full-resolution debug JPEG
showing the detections drawn on the scene. It runs on Jetson AGX Orin using a
YOLOv8s-OBB model compiled to a TensorRT engine.

There are two ways to run it: the **CLI binary** directly (one scene per
invocation, you control every flag), or the **Docker service** (a container
that watches a folder and processes whatever lands in it). The container does
not reimplement anything — it runs the exact same binary in a poll loop.

## 1. Repository layout

```
yolov8s-obb-cpp/
├── pipeline/                  C++/CUDA source, builds to sar_ship_detect
│   ├── build/                 CMake+Ninja build output (created by you)
│   ├── docker/                Dockerfile + entrypoint.sh for the service
│   └── README.md              design rationale (radiometry, tiling, land mask math)
├── data/                      the self-contained runtime data folder
│   ├── input/                 drop scenes (.tif) here
│   ├── output/                <scene_stem>.json (+ .jpg) appear here
│   ├── model/                 model_b16_640.onnx + cached .engine
│   └── GSHHS_shp/f/           GSHHS_f_L1..L6.shp (coastline polygons, land mask)
├── model.onnx                 original dynamic-shape export (do not run directly)
└── model_b16_640.onnx         shape-frozen export used throughout this manual
```

`pipeline/README.md` has the deep technical detail (why the radiometry window
is fixed, why tiles are 640px with 64px overlap, why land masking works the
way it does). This manual is the operational how-to-run-it guide; the README
is the "why it works this way" reference — where the two disagree, trust
whichever one matches the actual source, and file that as a bug in this
manual.

As of this writing, `data/input/` is empty and `data/output/` holds artifacts
from the last real run (`S1C_..._3271.json` / `.jpg`) — the scene file those
came from (`S1C_IW_GRDH_..._3271.tif`) was originally at the project root and
has since been removed from disk (not by anything in this manual's tooling),
so it's no longer available to re-run against. `data/model/` is unaffected —
`model_b16_640.onnx` and the cached `.engine` there are independent copies,
not symlinks.

## 2. Building the binary

```bash
cd pipeline
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

Requires `cmake`, `ninja-build`, `libgdal-dev`, and a matching CUDA +
TensorRT install (this box: CUDA 13.2, TensorRT 10.16.2, L4T R39.2 / JetPack
7.2). **If you have a conda environment active, deactivate it first** —
conda's GDAL is built against a different GEOS/libstdc++ ABI than the system
one, and the land-mask code leans on GEOS through `Buffer()`/`Intersection()`;
a mismatched build links silently and fails at runtime, not build time.

The target GPU architecture (SM 8.7 for Orin AGX) is set in
`CMakeLists.txt`, guarded to run *before* `project()` — CMake's own
auto-detected default gets cached the moment `project()` enables the CUDA
language, so setting it any later would silently be a no-op. If you build for
different hardware, pass `-DCMAKE_CUDA_ARCHITECTURES=<SM>` explicitly.

## 3. Running one scene by hand

```bash
./pipeline/build/sar_ship_detect \
  --tif  data/input/YOUR_SCENE.tif \
  --onnx data/model/model_b16_640.onnx \
  --engine data/model/model_b16_640.engine \
  --gshhg data/GSHHS_shp/f/GSHHS_f_L1.shp \
  --gshhg data/GSHHS_shp/f/GSHHS_f_L2.shp \
  --gshhg data/GSHHS_shp/f/GSHHS_f_L3.shp \
  --gshhg data/GSHHS_shp/f/GSHHS_f_L4.shp \
  --gshhg data/GSHHS_shp/f/GSHHS_f_L5.shp \
  --gshhg data/GSHHS_shp/f/GSHHS_f_L6.shp \
  --out-json data/output/YOUR_SCENE.json \
  --out-jpg  data/output/YOUR_SCENE.jpg \
  -v
```

**`--imgsz` and `--batch` are not needed here, and are worth understanding
rather than cargo-culting.** The actual tile size and batch size used at
runtime come from introspecting the loaded/built TensorRT engine
(`Engine::imgsz()`/`Engine::batch()`), which just reflects whatever shape was
already frozen into the ONNX — `Engine::build()` never reads `cfg.imgsz` or
`cfg.batch` at all. Passing `--imgsz` at a value that doesn't match the
engine only prints a warning (`engine imgsz N overrides --imgsz M`) and the
engine's value wins regardless; `--batch` isn't even compared, it's parsed
and then never read anywhere else in the codebase. In short: these two flags
currently do nothing functionally once an engine exists. The frozen shape is
fixed at export time (`model_b16_640.onnx` → batch 16, imgsz 640); if you
freeze a new export at different values, nothing you pass on this command
line changes that.

**First run only:** if `--engine` doesn't exist yet, the binary builds it
from the ONNX before doing anything else. This takes **~10+ minutes**
(measured: 693s on this box) and is tied to the exact GPU + TensorRT
version — delete and rebuild the `.engine` if either changes. Every
subsequent run reuses the cached plan and skips straight to inference
(sub-second engine load).

Drop `--out-jpg` to skip the debug render entirely — see §5 for why that
matters for speed.

### Full flag reference

| Flag | Default | Meaning |
|---|---|---|
| `--tif PATH` | *required* | float32 sigma0 VH GeoTIFF |
| `--onnx PATH` | *required* | shape-frozen YOLOv8-OBB ONNX |
| `--engine PATH` | `<onnx>.engine` | TensorRT plan cache; built if absent |
| `--imgsz N` | 640 | cosmetic only once an engine exists — see §3 |
| `--batch N` | 8 | parsed but never read anywhere — see §3 |
| `--overlap N` | 64 | tile overlap in px |
| `--streams N` | 2 | concurrent CUDA streams / TRT execution contexts |
| `--db-lo F` / `--db-hi F` | -25 / 0 | radiometry window (dB); change both together |
| `--conf F` | 0.50 | detection confidence threshold |
| `--nms-iou F` | 0.30 | rotated-IoU NMS threshold |
| `--max-det N` | 8192 | hard cap on detections |
| `--gshhg PATH` | none | GSHHS_f_L*.shp, repeat once per level |
| `--buffer-m F` | 100 | seaward buffer around land, metres |
| `--coarse-decim N` | 8 | land-raster decimation for the tile-skip accelerator (not listed in `--help`, but real) |
| `--no-skip-land-tiles` | off | run inference on land tiles too (debug use) |
| `--out-json PATH` | `detections.json` | detections output |
| `--out-jpg PATH` | *(empty = skip)* | full-res debug render |
| `--jpeg-quality N` | 90 | JPEG quality for the debug render |
| `--box-thickness N` | 5 | debug render box line width, px at full res |
| `--no-fp16` | off | build engine in FP32 instead of FP16 |
| `--workspace-mb N` | 4096 | TensorRT builder workspace |
| `-v, --verbose` | off | extra logging |

`--coarse-decim` and the fixed-true `skip_nodata_tiles` behavior (dropping
tiles that are entirely no-data / out of swath) have no other CLI toggle —
the nodata skip is always on, there's no `--no-skip-nodata-tiles`.

## 4. What happens internally, in order

1. **Scene read** — the whole GeoTIFF is read into pinned, GPU-mapped host
   memory in one `RasterIO` call (Orin's unified memory means the GPU can
   address it directly, no explicit H2D copy of the raw scene).
2. **Landmask build** and **engine load** run concurrently with the scene
   read (both are faster than the read on this scene, so they're fully
   hidden behind it) — GSHHG polygons from every given `--gshhg` level are
   each clipped to the scene and buffered outward by `--buffer-m`
   individually (not merged into one geometry); a point counts as land if it
   falls inside *any* of them. A low-resolution raster of the combined result
   is also built here, used only to accelerate tile skipping in step 3.
3. **Tile triage** — the scene is cut into tiles sized to the engine's frozen
   `imgsz` (640px in the model shipped here) at native 10m/px resolution,
   with `--overlap` (default 64px) between neighbours. Tiles that are
   entirely no-data (out of the SAR swath) or entirely inside the buffered
   land raster are skipped before inference — both are exact accelerators,
   not approximations, because a detection can't have its centre at sea
   inside a tile that contains no sea.
4. **Inference** — surviving tiles are batched (to the engine's frozen batch
   size, 16 here) and run through TensorRT across `--streams` concurrent
   execution contexts.
5. **Decode + rotated NMS** — raw boxes above `--conf` are decoded on GPU;
   detections in a tile's overlap margin are dropped in favor of the
   neighbouring tile that sees them more centrally; a global rotated-IoU NMS
   pass then dedupes across the whole scene.
6. **Land filter** — surviving detections whose *centre* falls inside the
   buffered land polygons are dropped, tested against exact vector geometry
   (a separate, precise check from the coarse raster used for tile skipping).
7. **Output** — `detections.json` is written; if `--out-jpg` was given, the
   full-resolution debug render happens **after** the JSON is already on
   disk (deliberately — it's the most expensive stage, and nothing
   downstream waits on it).

## 5. Debug JPEG: what it costs, and a fresh timing run

Re-ran the pipeline with `--out-jpg` on, using the scene in `data/input/`,
writing to `data/output/`:

| Stage | Time |
|---|---:|
| engine load *(cached, overlapped with read)* | 257 ms |
| landmask build *(overlapped with read)* | 694 ms |
| scene read *(excluded from hot path)* | 3,306 ms |
| tile triage | 29 ms |
| inference (628 tiles, 40 batches of 16) | 1,355 ms |
| rotated NMS | 0.6 ms |
| land filter | 171 ms |
| **HOT PATH (post-load)** | **1,585 ms** |
| debug render | 9,925 ms |
| **TOTAL** | **14,853 ms** (~14.9 s) |

Result: 90 detections at the default 100m buffer, consistent with every
prior run of this scene.

The debug render is **67% of total wall-clock**, more than 6x the actual
detection work (HOT PATH). The log line
`[jpeg] falling back to host libjpeg via GDAL` prints on every debug run —
`render.cpp` has a GPU-accelerated nvJPEG encode path that should be much
faster for an image this size (29508×21739, ~640 megapixels), but something
in `nvjpegCreateSimple`/`nvjpegEncoderStateCreate`/`nvjpegEncoderParamsCreate`
is failing before it ever reaches the actual encode call (no
`nvjpegEncodeImage failed` message appears in the log, which would print if
the failure were later than that). This has **not been root-caused**, only
observed and localized to "one of those three init calls" — flagging it here
because it's the single biggest lever if render speed ever matters
operationally. Until it's fixed: **only pass `--out-jpg` when you actually
need to look at the image.**

The resulting JPEG is also too large for most image viewers to decode
correctly (VS Code's built-in previewer, most browsers cap out well under
640 megapixels) — independently confirmed by decoding it through a separate
GDAL-based path and rendering a downscaled thumbnail, which showed a valid,
uncorrupted image. This is a viewer limitation, not file corruption.

## 6. Output format

This is the actual structure written by `main.cpp` — verified against a real
output file, not reconstructed from memory:

```json
{
  "scene": "S1C_IW_GRDH_1SDV_20260807T041243_20260807T041308_008884_0119F6_3271.tif",
  "acquisition_time": "2026-08-07T04:12:43Z",
  "sensing_start": "2026-08-07T04:12:43Z",
  "sensing_stop": "2026-08-07T04:13:08Z",
  "detection_count": 90,
  "detections": [
    {
      "time": "2026-08-07T04:12:43Z",
      "confidence": 0.8875916,
      "center": {"lon": 29.106831302, "lat": 44.086974714},
      "heading_deg": 126.756777302,
      "length_m": 226.875,
      "width_m": 109.53125,
      "corners_lonlat": [[...], [...], [...], [...]],
      "corners_pixel":  [[...], [...], [...], [...]]
    }
  ]
}
```

- `acquisition_time` and every per-detection `time` are the scene's sensing
  **start** (from the filename) — identical across all detections in a
  scene, not a per-detection timestamp. `sensing_start`/`sensing_stop` are
  both given at the top level if you need the full window.
- `heading_deg` is a 0–180 axis (long-axis true-north bearing), not a 0–360
  course — an oriented box carries no bow/stern information (see
  `detection.hpp`'s comment on `GeoDet::heading_deg`).
- Land masking uses each detection's **centre**, tested against exact vector
  geometry — a different, more precise check than the coarse raster used
  only to skip whole tiles before inference.

## 7. The Docker service

### How it works

```bash
docker run --runtime nvidia -v $(pwd)/data:/data sar-ship-detect
```

The host's `data/` folder (§1) is bind-mounted as `/data` inside the
container — same layout, same paths, nothing copied or renamed. Inside the
container, `pipeline/docker/entrypoint.sh`:

1. On start, checks whether `/data/model/model_b16_640.engine` exists and
   prints a heads-up log line if not. **This is informational only — it does
   not trigger a build.** The actual build happens lazily, the first time a
   scene is found and the binary is invoked on it; if `/data/input/` is
   empty at startup, no build happens until the first `.tif` arrives.
2. Every `POLL_SECONDS` (default 10s), lists `/data/input/*.tif`.
3. For each scene **without** a matching `/data/output/<stem>.json` already:
   - skips it if a `.{stem}.processing` marker from a previous attempt is
     still there (a crashed/killed run leaves this — it needs a human look,
     it is not auto-retried),
   - otherwise touches the marker and runs `sar_ship_detect` with fixed
     default paths for model/engine/GSHHG,
   - writes to `<stem>.json.tmp` and atomically renames to `<stem>.json` on
     success, so nothing ever reads a half-written file, then removes the
     marker,
   - on failure, leaves the marker in place and logs `FAILED <path>` to
     stderr — the scene is not retried on the next poll.
4. Sleeps, repeats forever. One bad scene never stops the loop — the `for`
   loop just continues to the next file.

Debug JPEGs are off by default (`DEBUG_JPG=0`) given the ~10s cost per scene
from §5. Set `-e DEBUG_JPG=1` on `docker run` to enable them for every scene
the service processes.

### Environment variables

| Variable | Default |
|---|---|
| `INPUT_DIR` | `/data/input` |
| `OUTPUT_DIR` | `/data/output` |
| `MODEL_DIR` | `/data/model` |
| `ONNX` | `$MODEL_DIR/model_b16_640.onnx` |
| `ENGINE` | `$MODEL_DIR/model_b16_640.engine` |
| `GSHHG_DIR` | `/data/GSHHS_shp/f` |
| `POLL_SECONDS` | `10` |
| `DEBUG_JPG` | `0` |
| `BIN` | `/app/sar_ship_detect` |

### Building the image

```bash
cd pipeline
docker build -f docker/Dockerfile -t sar-ship-detect .
```

**Before building**, confirm the base image tag in `docker/Dockerfile`
(`nvcr.io/nvidia/l4t-jetpack:r39.2.0`, currently an unverified placeholder)
actually exists on NGC for this box's exact version — L4T R39.2, JetPack
7.2-b187, confirmed via `cat /etc/nv_tegra_release` and
`dpkg -l | grep nvidia-l4t-core` on this host. This matters more than it
sounds: Jetson containers share the host's kernel GPU driver rather than
virtualizing it, and the cached `.engine` is locked to the exact GPU +
TensorRT build, so a version mismatch either fails outright or silently
rebuilds the engine on every container start. The runtime stage's GDAL
package name (`libgdal32`) is also a guess, not verified against this base
image's actual apt repo.

### Running it

```bash
docker run -d --name sar-detect \
  --runtime nvidia \
  -v $(pwd)/data:/data \
  sar-ship-detect
```

Requires `nvidia-container-runtime` configured as a Docker runtime on the
host — a host-level, one-time setup step, not something this project
controls.

### Operating it

```bash
docker logs -f sar-detect      # every stage's wall time prints here, per scene
docker stop sar-detect         # safe: no partial output files are ever visible
docker start sar-detect        # resumes; already-completed scenes are skipped
```

Drop a new `.tif` into `data/input/` at any time — it's picked up on the next
poll (≤10s later by default), no restart needed.

### Known gaps in the container setup (not yet resolved)

- Base image tag is unverified against NGC's actual catalog.
- Runtime GDAL package name in the Dockerfile is a guess.
- No `HEALTHCHECK` — an orchestrator can't currently distinguish "still
  building the engine on first start" from "hung."
- The container itself has never actually been built or run end-to-end —
  only the entrypoint script's logic has been tested directly against the
  compiled binary, outside Docker.

## 8. Known gaps in the detection pipeline itself

(carried over from `pipeline/README.md`, still true)

- The output channel layout `[cx, cy, w, h, score, angle]` is inferred from
  export metadata, not verified against a live model run.
  `Engine::introspect()` only checks the channel *count* (must be 6) — it
  cannot detect a reordering of those six values.
- No validation set — detection quality has only been assessed by eye on the
  debug JPEG.
- A 100m land buffer will not suppress moored vessels inside a real harbor
  basin (GSHHG doesn't map quays/breakwaters). Constanța sits in the test
  scene; direct comparison gave 90 detections at 100m vs. 88 at 200m — the
  2 extra drops are vessels just off open coastline, not inside the port
  basin itself, which needs a much larger buffer (500m–1km) or a separate
  port-polygon layer to suppress.
