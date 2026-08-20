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

## 2b. Preparing a DEM (only needed for raw `.SAFE` input)

Terrain correction needs a DEM covering the scene **and** a geoid grid. Both go
in `data/dem/`, and both are mounted read-only into the container:

```
data/dem/
├── dem.tif      elevation, any GDAL-readable format and any CRS
└── egm96.tif    geoid undulation, added to the DEM
```

The names are `DEM_NAME` / `GEOID_NAME` in `run.sh` if you want different ones.
Anything not already in EPSG:4326 is reprojected on the fly through a warped
VRT, so a UTM DEM is fine as-is.

For SRTM `.hgt` tiles, a VRT avoids mosaicking anything to disk:

```bash
gdalbuildvrt data/dem/dem.tif.vrt /path/to/*.hgt && mv data/dem/dem.tif.vrt data/dem/dem.tif
```

The geoid grid is PROJ's `us_nga_egm96_15.tif` (about 2 MB), which ships in
`proj-data`; copy it to `data/dem/egm96.tif`.

**Do not skip the geoid.** SRTM heights are orthometric and the range-Doppler
solve needs ellipsoidal ones. In the Black Sea that difference is +35 m, which
is a measured ~4 pixel geolocation error — see `pipeline/README.md` for the
number and how it was measured. `--dem` without `--geoid` is a hard error unless
you pass `--dem-is-ellipsoidal`.

If you have no DEM at all, pass a `.tif` that is already terrain corrected (the
original workflow), or omit `--dem` to geocode on the ellipsoid — exact over
water, land displaced by height/tan(incidence).

## 3. Running one scene

**The supported way is `./run.sh`, which runs everything in the container:**

```bash
./run.sh detect data/input/YOUR_SCENE.SAFE -o data/output   # raw L1 -> ships
./run.sh detect data/input/YOUR_SCENE.tif  -o data/output   # already-L2 -> ships
```

A `.SAFE` directory or `.zip` is a raw Sentinel-1 L1 GRD product: it is
calibrated, thermal-noise corrected and range-Doppler geocoded on the GPU
first, in the same process, with no intermediate file. That needs `data/dem/`
per §2b. A `.tif` is taken to be already terrain corrected and goes straight to
detection, exactly as before.

`run.sh` picks the input mode from the extension, mounts what each mode needs,
and forwards any other flags to the binary. Useful ones:

| flag | effect |
|---|---|
| `--save-l2` | also write `<stem>_L2.tif`, the geocoded sigma0 (2.5 GB, slow) |
| `--save-render` | also write the full-resolution debug JPEG (slow, see §5) |
| `--no-crops` / `--no-overview` | turn off the defaults |
| `POL=VV ./run.sh …` | process VV instead of VH |

### Running the binary directly

Only useful when you are working on the pipeline itself; `run.sh` is what the
service and the docs assume.

```bash
./pipeline/build/sar_ship_detect \
  --safe data/input/YOUR_SCENE.SAFE --pol VH \
  --dem data/dem/dem.tif --geoid data/dem/egm96.tif \
  --onnx data/model/model_b16_640.onnx \
  --engine data/model/model_b16_640.engine \
  --gshhg data/GSHHS_shp/f/GSHHS_f_L1.shp \
  --out-json data/output/YOUR_SCENE.json \
  --out-overview data/output/YOUR_SCENE_overview.jpg \
  --out-crops data/output/YOUR_SCENE_crops
```

The `--tif` form is unchanged:

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
  --out-jpg  data/output/YOUR_SCENE.jpg
```

The tile size and batch size used at runtime always come from introspecting
the loaded/built TensorRT engine (`Engine::imgsz()`/`Engine::batch()`), which
just reflects whatever shape was frozen into the ONNX — there's no CLI flag
for either, since one couldn't change the engine's actual shape anyway. The
frozen shape is fixed at export time (`model_b16_640.onnx` → batch 16, imgsz
640); freeze a new export if you need different values.

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
| `--tif PATH` | *one of these two is required* | float32 sigma0 GeoTIFF, already terrain corrected |
| `--safe PATH` | *one of these two is required* | raw Sentinel-1 L1 GRD `.SAFE` or `.zip` |
| `--pol VH\|VV` | VH | polarisation, `--safe` only |
| `--dem PATH` | *(none)* | DEM for terrain correction; omit for ellipsoid-only |
| `--geoid PATH` | *(none)* | geoid undulation grid; required with `--dem`, see §2b |
| `--dem-is-ellipsoidal` | off | assert the DEM already has ellipsoidal heights |
| `--pixel-spacing F` | 10 | output metres per pixel |
| `--epsg N` | 0 = the scene's UTM zone | output CRS |
| `--coarse-step N` | 32 | geocoding lattice step, output px |
| `--out-l2 PATH` | *(empty = skip)* | also write the geocoded sigma0 GeoTIFF |
| `--onnx PATH` | *required* | shape-frozen YOLOv8-OBB ONNX |
| `--engine PATH` | `<onnx>.engine` | TensorRT plan cache; built if absent |
| `--overlap N` | 64 | tile overlap in px |
| `--streams N` | 2 | concurrent CUDA streams / TRT execution contexts |
| `--db-lo F` / `--db-hi F` | -25 / 0 | radiometry window (dB); change both together |
| `--conf F` | 0.50 | detection confidence threshold |
| `--nms-iou F` | 0.30 | rotated-IoU NMS threshold |
| `--max-det N` | 8192 | hard cap on detections |
| `--gshhg PATH` | none | GSHHS_f_L*.shp, repeat once per level |
| `--buffer-m F` | 100 | seaward buffer around land, metres |
| `--coarse-decim N` | 8 | land-raster decimation for the tile-skip accelerator (not listed in `--help`, but real) |
| `--out-json PATH` | `detections.json` | detections output |
| `--out-jpg PATH` | *(empty = skip)* | full-res debug render — slow, see §5 |
| `--out-overview PATH` | *(empty = skip)* | decimated whole-scene render with boxes |
| `--overview-max N` | 4096 | longest side of the overview |
| `--out-crops DIR` | *(empty = skip)* | water crops + manifest, see §5b |
| `--crop-size N` | 1024 | square crop side, px |
| `--crop-quality N` | 80 | JPEG quality for the full-res crop |
| `--crop-thumb N` | 256 | thumbnail side, px (0 disables the tier) |
| `--crop-min-sea F` | 0 | fraction of a crop's sea allowed to go uncovered |
| `--crop-coast-lo F` | 0.02 | land fraction above which a crop counts as coastal |
| `--no-crop-coast` | off | skip the extra ~50/50 shoreline crops |
| `--crop-threads N` | 0 = cores−2 | crop encode threads |
| `--jpeg-quality N` | 90 | JPEG quality for the debug render and overview |
| `--box-thickness N` | 5 | debug render box line width, px at full res |
| `--workspace-mb N` | 4096 | TensorRT builder workspace |

`--coarse-decim` and the fixed-true `skip_nodata_tiles` behavior (dropping
tiles that are entirely no-data / out of swath) have no other CLI toggle —
the nodata skip is always on, there's no `--no-skip-nodata-tiles`.

## 4. What happens internally, in order

0. **L1 → L2, only with `--safe`** — the product's annotation is parsed for the
   orbit, azimuth timing, slant-to-ground polynomials and the calibration and
   noise LUTs; the uint16 DN raster is read; one CUDA kernel turns it into
   sigma0 with thermal noise removed; a second range-Doppler geocodes it onto a
   UTM grid using the DEM. The result lands directly in the scene buffer — no
   intermediate GeoTIFF is written or read back, which is where most of the
   time SNAP spends on this actually goes. Validated against SNAP; see
   `pipeline/README.md`.
1. **Scene read** — with `--tif`, the whole GeoTIFF is read into pinned,
   GPU-mapped host memory (Orin's unified memory means the GPU can address it
   directly, no explicit H2D copy of the raw scene). With `--safe` this step
   does not happen: step 0 already filled that buffer.
2. **Landmask build** and **engine load** run concurrently with the scene
   read (both are faster than the read on this scene, so they're fully
   hidden behind it) — GSHHG polygons from every given `--gshhg` level are
   each clipped to the scene and buffered outward by `--buffer-m`
   individually (not merged into one geometry); a point counts as land if it
   falls inside *any* of them. A low-resolution raster of the combined result
   is also built here, used to accelerate tile skipping in step 4 and to
   classify crops in step 5.
3. **Quantise** — one GPU pass converts the float32 scene to the 8-bit values
   the model is shown, and fills a decimated "has imagery" mask at the same
   time. Every later stage reads those bytes instead of re-deriving them from
   the float raster. See `pipeline/README.md` for why this is bit-identical
   rather than an approximation.
4. **Tile triage** — the scene is cut into tiles sized to the engine's frozen
   `imgsz` (640px in the model shipped here) at native 10m/px resolution,
   with `--overlap` (default 64px) between neighbours. Tiles that are
   entirely no-data (out of the SAR swath) or entirely inside the buffered
   land raster are skipped before inference — both are exact accelerators,
   not approximations, because a detection can't have its centre at sea
   inside a tile that contains no sea.
5. **Water crops dispatched** — if `--out-crops` was given, crop planning and
   JPEG encoding start now on CPU worker threads. Neither needs a detection,
   so both run underneath step 6 on cores that would otherwise sit waiting on
   the GPU. Nothing is collected until step 9.
6. **Inference** — surviving tiles are batched (to the engine's frozen batch
   size, 16 here) and run through TensorRT across `--streams` concurrent
   execution contexts.
7. **Decode + rotated NMS** — raw boxes above `--conf` are decoded on GPU;
   detections in a tile's overlap margin are dropped in favor of the
   neighbouring tile that sees them more centrally; a global rotated-IoU NMS
   pass then dedupes across the whole scene.
8. **Land filter** — surviving detections whose *centre* falls inside the
   buffered land polygons are dropped, tested against exact vector geometry
   (a separate, precise check from the coarse raster used for tile skipping).
   The polygons are GEOS-prepared, so this is an indexed test rather than a
   linear walk of a coastline with hundreds of thousands of vertices.
9. **Output** — `detections.json` is written, then the crop workers are joined
   and their manifest written (it needs the detection list to say which
   detections fall in which crop). The overview render follows, and the
   full-resolution debug render last of all — deliberately, because it's the
   most expensive stage and nothing downstream waits on it.

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

### What changed since that table was measured

Those numbers are from before the crop/overview work and have **not** been
re-measured on hardware — nobody has run the new binary on a Jetson yet. What
changed, and why each should move:

- **`--out-jpg` should get materially faster** without nvJPEG being fixed. The
  host fallback no longer goes through a GDAL MEM dataset and three
  interleave-strided `RasterIO` passes before libjpeg sees a scanline; it hands
  the interleaved buffer straight to libjpeg-turbo. The render kernel also no
  longer recomputes `10*log10` for 640 Mpx — it expands the bytes step 3
  already produced.
- **The nvJPEG failure is now diagnosable.** Each of the three init calls
  reports its own status name. If `nvjpegEncoderStateCreate` returns
  `ARCH_MISMATCH`, that is this L4T build having no nvJPEG *encoder* for Orin,
  and the host path is the only path — stop chasing it.
- **Inference should get a little faster**: preprocess reads 1 byte per pixel
  instead of 4 and does no `log10`, and it no longer recovers tile/row indices
  by dividing a flat counter (three runtime integer divisions per pixel, at
  640×640×16 per batch).
- **Land filter, 171 ms, should collapse** to single-digit milliseconds via
  GEOS prepared geometry.
- **Tile triage** no longer runs its own GPU sweep; it reads the mask step 3
  already produced.
- **Quantise is a new cost**, one pass over the scene, and should land in the
  tens of milliseconds.

Re-measure before quoting any of this. The one number worth watching is still
`HOT PATH (post-load)`.

## 5b. Water crops

`--out-crops DIR` writes the water of the scene as standalone grayscale JPEGs:

```
DIR/
├── manifest.json                    every crop, where it is, what is in it
├── <stem>_x18432_y13312.jpg         1024x1024 grayscale, quality 80
├── ...
└── thumbs/
    └── <stem>_x18432_y13312.jpg     256x256, quality 65
```

Land-only and out-of-swath windows are never written. Crops along the shoreline
are slid onto the coast so they come out about half water and half land; the
grid crop underneath is kept too, so no water is left uncovered. The full
rationale, and the measured trade-off behind the 1024 default, is in
`pipeline/README.md`.

Each manifest entry carries the crop's pixel origin, its WGS84 corners, its sea
and land fractions, its byte count, its **CRC-32**, and the indices of the
detections whose centres fall inside it. Boxes are not burnt into the pixels —
the ground draws them from the manifest, and the imagery stays clean for
anything else you want to run on it.

Rough expectation for a GRDH scene like the one in §5 (628 sea tiles ⇒ ~200 Mpx
of water ⇒ ~200 crops at 1024): **30–45 MB** of full-resolution crops and
**~2.5 MB** of thumbnails. That is an estimate from the tile count and typical
JPEG rates on SAR speckle, not a measurement.

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
      "heading_confidence": 0.421875,
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
- `heading_deg` is a 0–360 true-north bearing, bow-first. The OBB itself only
  gives a 0–180 axis, so which end is the bow is picked by a GPU shape
  heuristic (`k_estimate_heading` in `detect/kernels.cu`) — see
  `detection.hpp`'s comment on `GeoDet::heading_deg` and `pipeline/README.md`.
  `heading_confidence` (0–1) says how asymmetric the two ends looked; treat
  anything below ~0.3 as an unconfirmed bow guess, not a course to act on.
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
2. Every `POLL_SECONDS` (default 10s), lists `/data/input/*.tif`, `*.tiff`,
   `*.SAFE` and `*.zip`. The last two are raw L1 products and go through the
   on-GPU L1→L2 stage first; they are skipped with a clear log line if no DEM
   is present.
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

Water crops and the overview are **on** by default — together they are what the
ground station actually looks at, and both are cheap (the crops encode on CPU
threads underneath the GPU inference stage; the overview is a fraction of a
second). Turn them off per-container with `-e CROPS=0` / `-e OVERVIEW=0`.

### Environment variables

| Variable | Default |
|---|---|
| `INPUT_DIR` | `/data/input` |
| `OUTPUT_DIR` | `/data/output` |
| `MODEL_DIR` | `/data/model` |
| `ONNX` | `$MODEL_DIR/model_b16_640.onnx` |
| `ENGINE` | `$MODEL_DIR/model_b16_640.engine` |
| `GSHHG_DIR` | `/data/GSHHS_shp/f` |
| `DEM_DIR` | `/data/dem` |
| `DEM` | `$DEM_DIR/dem.tif` — only used for `.SAFE` input |
| `GEOID` | `$DEM_DIR/egm96.tif` |
| `POL` | `VH` |
| `POLL_SECONDS` | `10` |
| `DEBUG_JPG` | `0` — the slow full-res render, off |
| `CROPS` | `1` — water crops, on |
| `OVERVIEW` | `1` — decimated overview, on |
| `CROP_SIZE` | `1024` |
| `CROP_QUALITY` | `80` |
| `CROP_THUMB` | `256` |
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

## 7b. The resident detector (what `run.sh detect` actually talks to)

Section 7's watch-folder service starts a fresh process per scene. That pays
~430 ms of dynamic linking, CUDA context creation and engine load every time.
The resident detector pays it once and then stays up.

`docker-compose.yml` starts the container with `/app/serve_supervisor.sh` as its
entrypoint, so the resident process is the container's own main process:

- `restart: unless-stopped` covers it, which means a crash of the container, a
  Docker restart or a host reboot all bring it back with no human involved.
- The supervisor relaunches the detector if the detector alone dies, with a
  backoff of 30 s if it dies more than 5 times in a minute.
- The detector deliberately exits after a job that threw, and lets the
  supervisor hand back a fresh process. That is on purpose: an aborted job
  skips the `cudaFree` calls at the end of `runOnce`, so continuing in the same
  process would leak a few hundred MB of GPU memory per failure. Recycling
  costs ~0.5 s once and leaks nothing.

Requests arrive over two named pipes in `data/.serve/` (`req` and `resp`).
`run.sh detect` uses them only when the caller's uid matches the uid the
detector runs as (root, `SERVE_UID`), so a manual run as your own user still
goes down the `docker exec` path and leaves the output files owned by you.

### Operating it

```bash
./run.sh up             # container + resident detector, waits until ready
./run.sh status         # container, supervisor, detector, fifos, fast path
./run.sh serve-log -f   # the resident detector's own log
./run.sh serve-restart  # hand back a freshly started detector
./run.sh serve-stop     # stop the detector, keep the container up
./run.sh serve-start    # undo serve-stop
./run.sh down           # stop the container
```

`status` exits 0 only when the detector is actually usable, so it works in a
health check. A typical healthy line:

```
serve       ready      pid=73 up=3m48s jobs=9 last=rc0/3738ms 8s ago
```

`phase` is `ready` between jobs and `busy` during one. `jobs` and `last` come
from `data/.serve/stats`, which the detector rewrites after every job, so they
distinguish "the process exists" from "the process is still doing work".

`serve-stop` writes `data/.serve/stop`; the supervisor sees it and idles instead
of relaunching. The container stays up, so `run.sh detect` keeps working through
`docker exec` — slower by the startup cost, not broken.

### The listener owns the detector's lifetime

`baklava-listener` starts the resident detector before it answers its first
request, and stops it on the way out, so stopping the listener really does
release the GPU on this shared Jetson:

```bash
docker start baklava-listener   # -> sar-ship-detect comes up too
docker stop  baklava-listener   # -> sar-ship-detect goes down too (~0.5 s)
```

PID 1 of the listener container is `jetson/listener_entrypoint.sh`, wired in
through `command:` in `jetson/docker-compose.yml`. It is read from the bind
mount, so editing it needs no image rebuild — unlike `listener_service.py`,
which is baked into the image.

On the way down it SIGTERMs the Flask process first and gives it up to 20 s to
finish an in-flight scene, and only then stops the detector. The listener's
`stop_grace_period` is 60 s to leave room for both.

It calls `run.sh detector-start` / `detector-stop` rather than `run.sh up` /
`down`, because **the listener image has the docker CLI but not the compose
plugin** — `docker compose` inside that container fails with "unknown command".
Those two subcommands use plain `docker start` / `docker stop` for that reason,
and need the container to already exist.

So the one-time bootstrap on a fresh host is still `./run.sh up`, which is what
creates the container. If the container has been removed (`./run.sh down`), the
listener logs

```
WARNING detector did not start; detect calls fall back to a one-shot container
```

and starts anyway — detection still works, just without the resident process.

### Failure behaviour

- A job that fails returns its own exit status to the caller. It is not retried
  down the `docker exec` path; only an unreachable detector triggers the
  fallback, and that is capped at 1 s.
- If the detector dies mid-job — including through `CUDA_CHECK`, which calls
  `exit()` and cannot be caught — an `atexit` handler still answers the waiting
  caller, so a client never sits on the 300 s response timeout.
- If a caller disappears (Ctrl-C) the detector gives up on the response pipe
  after 10 s and goes back to serving instead of blocking forever.

## 8. Getting it to the ground

The Jetson runs `jetson/listener_service.py`; the ground runs
`jetson_client.sh`. Only the JSON used to cross that link. Now:

```bash
./jetson_client.sh overview SCENE__cpp              # one small JPEG, whole scene
./jetson_client.sh crops    SCENE__cpp              # manifest + thumbnails (default)
./jetson_client.sh crops    SCENE__cpp --full       # full-resolution crops
./jetson_client.sh crops    SCENE__cpp --all        # both tiers
./jetson_client.sh crops    SCENE__cpp --full --only-detections
./jetson_client.sh crops    SCENE__cpp --all --tar  # one streamed archive
```

The intended order of operations is thumbnails first (~2.5 MB for a whole
scene), look, then pull the full-resolution crops you actually want. Nothing on
board is discarded either way — the tiering is about what crosses the link.

**`crops` is incremental and self-healing.** It fetches the manifest, checksums
what is already on disk, and downloads only what is missing or wrong. An
interrupted transfer leaves a part file keyed by the CRC it is being assembled
towards, so the next run resumes it byte-range-wise rather than starting over —
and a leftover part file from a *different* crop can never be resumed into,
which is the failure a byte-count check cannot see (resuming into stale bytes
lands on exactly the right file size). Every completed file is checksummed
before it is promoted; anything that fails is discarded and refetched. Re-run
the same command after a failure and it costs only the remainder.

`--tar` swaps that for a single streamed, uncompressed archive: faster on a good
link, but it restarts from scratch if the link breaks. Uncompressed because the
members are already JPEG — a gzip layer would burn CPU on both ends to add bytes.

### Endpoints

| Endpoint | Returns |
|---|---|
| `GET /scenes/<name>/overview` | decimated whole-scene JPEG |
| `GET /scenes/<name>/crops` | `manifest.json` |
| `GET /scenes/<name>/crops/<relpath>` | one crop or thumbnail (Range-capable) |
| `GET /scenes/<name>/crops.tar?tier=thumb\|full\|all&only=detections` | streamed tar |

`<name>` is the outbox scene name, i.e. `<stem>__cpp` for the cpp backend.
Imagery is published into `BASE/imagery/` by symlink, not copy — the detector's
output directory stays the only copy on the device.

**Crops are a cpp-backend feature.** The legacy Python backend
(`BAKLAVA_BACKEND=legacy`) produces neither crops nor an overview, and asking
for them returns 404.

`BAKLAVA_BASE`, `JETSON_HOST` and `JETSON_PORT` are now environment overrides
rather than hardcoded, which is what makes the pair testable off the device.

## 9. Known gaps in the detection pipeline itself

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
- **The L1→L2 stage reproduces SNAP's arithmetic, not all of its steps.** No
  Apply-Orbit-File (the annotated restituted orbit is used, which is what would
  be available on board at acquisition time), no Remove-GRD-Border-Noise, no
  layover/shadow mask, GRD only. `pipeline/README.md` lists each and why.
- **Nothing added for the L1→L2 stage, crops or overview has run on a Jetson
  yet.** The parsing and the geometry are validated against SNAP on the host —
  `pipeline/tests/run.sh` reproduces those numbers — but the two new CUDA
  kernels have never been compiled, because nothing here has CUDA. Their host
  twins in the test are what was checked. Expect the first Jetson build to need
  fixing.
- **The L1→L2 stage has not been timed.** The design targets seconds: two
  memory-bound kernels over ~440 Mpx and no intermediate file. That is a
  prediction, not a measurement.
- **Nothing added for crops/overview has run on a Jetson yet.** The crop
  planner and encoder were built and tested off-device against synthetic
  coastlines, and the service/client pair was tested end to end over real HTTP
  against a fake on-board tree (including resume, checksum mismatch and path
  traversal). The CUDA changes — quantise, the reworked preprocess and render,
  the overview kernel — have not been compiled, because nothing here has CUDA
  or TensorRT. Expect the first Jetson build to need fixing.
- The GEOS prepared-geometry path is guarded on `GDAL_VERSION_NUM >= 3.3` and
  falls back to plain `Contains()` below that. This box has GDAL 3.8, so the
  fallback is untested here.
- `--crop-min-sea 0` (the default) guarantees no water is dropped, but "water"
  means a cell of the `--coarse-decim` mask — 80 m at the default. Slivers
  finer than that are invisible to the planner, as they already were to the
  tile-skip test.
