# SAR ship detection — YOLOv8s-OBB on Jetson AGX Orin

Sentinel-1 GRD → oriented vessel detections with WGS84 coordinates, water
crops, and an overview render. Either from a raw L1 `.SAFE` product (`--safe`,
which does calibration, thermal-noise removal and range-Doppler terrain
correction on the GPU) or from an already-terrain-corrected float32 sigma0
GeoTIFF (`--tif`, the original path).

Target: JetPack 7.2 / L4T R39.2, CUDA 13.2, TensorRT 10.16.2, SM 8.7.

## Layout

Source is grouped by what it does, not left flat:

```
pipeline/
├── main.cpp             orchestration: parse Config, run every stage in order
├── config.hpp/.cpp       CLI flags -> Config
├── common/               leaf types/macros used everywhere
│   ├── detection.hpp       Det (raw pixel-space), GeoDet (geo-referenced output)
│   └── util.hpp             CUDA_CHECK, FATAL, Timer
├── io/                   reading external data sources
│   ├── scene.hpp/.cpp       owns the raster in pinned/GPU-mapped memory, geo transforms
│   ├── landmask.hpp/.cpp    GSHHG shapefiles -> buffered exclusion polygons
│   └── safe.hpp/.cpp        parses a raw Sentinel-1 .SAFE product's XML/LUTs
├── l1/                   L1 -> L2 SAR processing (only exercised by --safe)
│   ├── rdgeom.hpp/.cpp      orbit interpolation, zero-Doppler solve, ECEF math
│   ├── l1proc.hpp/.cpp      orchestrates calibrate -> geocode -> Scene
│   └── kernels.cuh/.cu      k_calibrate, k_rd_geocode
├── detect/               the detection pipeline
│   ├── engine.hpp/.cpp      TensorRT wrapper: load/build plan, execution contexts
│   └── kernels.cuh/.cu      k_quantise, k_preprocess, k_decode, rotated NMS
├── output/                everything that turns detections into files
│   ├── render.hpp/.cpp      full-res debug JPEG (nvJPEG, falls back to libjpeg)
│   ├── jpeg.hpp/.cpp         plain libjpeg encoder (writeJpegGray/RGB)
│   ├── crops.hpp/.cpp        per-window water crop export + manifest
│   └── kernels.cuh/.cu      k_render_rgb, k_overview_rgb, k_draw_boxes
└── docker/                containerized folder-watcher service
```

Each `kernels.cu` is scoped to its directory's concern (L1 geocoding math,
core detection, or rendering) — there's no single flat kernel file to search
through, and each is its own small CUDA compilation unit.

## Build

```bash
sudo apt install -y cmake ninja-build libgdal-dev libtiff-dev libjpeg-dev pkg-config
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

## Prepare the model

The exported ONNX has dynamic batch/height/width. Freeze it first — static
shapes mean TensorRT builds without an optimisation profile, which is where
most of its latency jitter comes from.

```bash
python3 04_freeze_onnx_shapes.py model.onnx -o model_b8_640.onnx --batch 8 --imgsz 640
```

Build the plan once, offline. It is tied to this exact GPU and TensorRT
version; if either changes, delete the `.engine` and rebuild.

```bash
./build/sar_ship_detect --tif scene.tif --onnx model_b8_640.onnx \
    --engine model_b8_640.engine --out-json /dev/null
```

## Run

```bash
./build/sar_ship_detect \
  --tif  S1C_IW_GRDH_1SDV_20260807T041243_20260807T041308_008884_0119F6_3271.tif \
  --onnx model_b8_640.onnx \
  --engine model_b8_640.engine \
  --gshhg /data/gshhg/GSHHS_shp/f/GSHHS_f_L1.shp \
  --gshhg /data/gshhg/GSHHS_shp/f/GSHHS_f_L2.shp \
  --gshhg /data/gshhg/GSHHS_shp/f/GSHHS_f_L3.shp \
  --gshhg /data/gshhg/GSHHS_shp/f/GSHHS_f_L4.shp \
  --gshhg /data/gshhg/GSHHS_shp/f/GSHHS_f_L5.shp \
  --gshhg /data/gshhg/GSHHS_shp/f/GSHHS_f_L6.shp \
  --out-json     detections.json \
  --out-overview overview.jpg \
  --out-crops    crops/
```

Every stage prints its wall time to stderr. `HOT PATH (post-load)` is the number
to watch — it excludes the GeoTIFF read, per the agreed budget.

## L1 → L2 on the GPU

`--safe PATH` takes a raw Sentinel-1 GRD product — a `.SAFE` directory or a
`.zip`, read in place through `/vsizip/` — and produces the geocoded sigma0
raster the detector wants, in memory, in one process.

It replaces the SNAP chain in `image_transformation/sar_l1_to_l2.py` for the
on-board case. SNAP is still the reference implementation and still the right
tool off-board; this is the version that has to run in seconds on a Jetson.

```
     annotation XML          measurement TIFF (uint16 DN)
     orbit, timing,                    |
     srgr polynomials,                 v
     cal + noise LUTs  ---->  [ k_calibrate ]      sigma0 = (DN^2 - noise) / A^2
     (io/safe.cpp)                     |
                                       v
     DEM + geoid       ---->  [ k_rd_geocode ]     zero-Doppler + srgr + resample
     coarse lattice                    |
     (l1/l1proc.cpp)                   v
                              scene buffer  ->  detector, no file in between
```

Two kernels, no intermediate product. That last point is most of the speedup:
SNAP's chain writes and re-reads several gigabytes between every stage, and the
original workflow then wrote a 2.5 GB L2 GeoTIFF that the detector immediately
read back. `--out-l2` still writes one if you want it, and it is the single
most expensive thing in the stage when you do.

### What each step is

**Calibration and thermal noise.** `sigma0 = max(DN² − noise, 0) / sigmaNought²`,
with the sparse LUTs from the annotation expanded to one value per range sample
on the host so the kernel does no searching. Out-of-swath samples are DN 0, so
the noise subtraction drives them negative and they clamp to 0 — the same value
SNAP writes there, with no separate nodata test.

**Range-Doppler terrain correction.** One thread per output pixel: take the
target's ECEF position, solve `(Psat(t) − T) · Vsat(t) = 0` for the zero-Doppler
azimuth time, turn the slant range into a ground range through the product's own
`srgrCoefficients` polynomial, and bilinearly resample.

Three things make that cheap enough to do per pixel:

- **A coarse lattice, solved exactly on the host.** Every 32nd output pixel gets
  an exact solve; the kernel bilinearly interpolates it as a seed and Newtons
  three times. The seed is always within a fraction of a line.
- **ECEF on the lattice, not lat/lon.** Geodetic-to-ECEF is *linear in height
  along the ellipsoid normal*, so `T(h) = T(0) + h·u` exactly. Storing `T(0)`
  and `u` at the lattice nodes means terrain height costs one multiply-add and
  there is no trigonometry in the kernel at all.
- **A 1 ms orbit table.** Lagrange-8 interpolation of the 17 state vectors is
  done once on the host into a uniform table; the kernel interpolates that
  linearly, where the error is `|a|·dt²/8 ≈ 1e-6 m`.

Positions are stored relative to the scene centre so the kernel can work in
float: absolute ECEF is ~6.4e6 m, where float resolution is 0.4 m, but
everything relative to that origin is under ~1000 km, where it is 0.05 m. The
slant-to-ground polynomial stays in double — its high-order coefficients run
down to 1e-39, which is denormal in float.

### The geoid is not optional

SRTM heights — and most other DEMs — are **orthometric**: metres above the
geoid. The range-Doppler solve needs **ellipsoidal** height. In the western
Black Sea the EGM96 undulation is about +35 m, and skipping it puts every pixel
about 4 px out.

This is measured, not asserted. Sweeping the assumed height and correlating
against SNAP's own terrain-corrected output peaks sharply at 35 m:

| assumed height | speckle correlation with SNAP |
|---:|---:|
| 0 m | 0.025 |
| 25 m | 0.477 |
| 30 m | 0.811 |
| **35 m** | **0.999** |
| 40 m | 0.836 |
| 50 m | 0.222 |

So `--dem` without `--geoid` is a hard error unless you pass
`--dem-is-ellipsoidal`. Omitting `--dem` entirely is allowed and geocodes on the
ellipsoid: exact over water, land displaced by height/tan(incidence).

### Validation

`io/safe.cpp` and `l1/rdgeom.cpp` were checked against the real product and
against SNAP, four ways:

| check | result |
|---|---|
| geocoding round-trips the product's own 210-point geolocation grid | 0.19 px azimuth, 0.16 px range worst case |
| calibration + noise vs SNAP's `Sigma0_VH` band, 5 windows | correlation 1.00000000, ratio 1.000000 |
| geocoding vs SNAP's terrain-corrected output, open water | 0.99946 speckle correlation |
| that correlation peaks at the EGM96 undulation | peak at 35 m |

Correlating *individual speckle pixels* at 0.999 is a sub-0.1-pixel test:
speckle is essentially a random field, so nothing but correct geometry produces
that number. The harness that produced these numbers is not checked into this
repo — treat the table as a result to reproduce, not a command to run.

### What it does not do

- **SLC.** GRD only. `io/safe.cpp` hard-fails on anything whose annotated
  projection is not `Ground Range`, rather than geocoding it wrongly.
- **Apply-Orbit-File.** The annotated restituted orbit is used as-is. The
  precise orbit is published ~20 days later and is not going to exist on board
  at acquisition time. Restituted is good to a few centimetres for this purpose.
- **Remove-GRD-Border-Noise.** Not reproduced. For IPF 2.9+ products the
  improved noise LUTs largely cover it; on older products expect some bright
  ragged samples at the extreme swath edges.
- **Terrain flattening (RTC), multilooking, speckle filtering.** All deliberately
  absent, exactly as in the SNAP recipe — see `sar_l1_to_l2.py` on why each is
  wrong for water.
- **Layover/shadow masking.** Not computed.

## Radiometry

Fixed global transfer function, no per-tile adaptation:

```
g   = clamp((10*log10(max(sigma0, 1e-12)) - (-25)) / (0 - (-25)), 0, 1)
u8  = round(g * 255)
in  = u8 / 255                      # replicated across R, G, B
```

The 8-bit round trip is deliberate: the model was trained on JPEG renders, so
quantising reproduces the input distribution it actually saw. The debug JPEG
uses the identical function, so what you look at is what the network looked at.

Change the window with `--db-lo` / `--db-hi` — but change it in both places at
once, which is automatic here since they share one constant.

### The `u8` is computed once, for everything

`launch_quantise` (in `detect/kernels.cu`) runs one pass over the float32
scene and keeps the `u8` line above as a `W*H` byte buffer. Tile preprocess,
the water crops, the overview and the full render then all read that buffer
instead of re-deriving it:

- preprocess used to re-read float32 for every tile, and tiles overlap by 64 px,
  so a strip of the scene paid for `log10` twice
- the render used to read the entire float32 scene a second time

Both are memory-bound on an integrated GPU, so going from 4 bytes and a
transcendental per pixel to 1 byte is close to a 4x cut in their traffic. It is
also bit-identical, not an approximation: the quantisation was already in the
old code paths, and both of them already rounded to exactly this value.

The same pass also fills a `--coarse-decim`-decimated **data mask** (a cell is
set if any pixel in it has `sigma0 > 0`). Tile triage reads that instead of
running its own sweep over the scene, and the crop planner classifies sea versus
out-of-swath from it.

Memory cost: `W*H` bytes, 641 MB on a GRDH scene. It is allocated mapped and
pinned like the scene raster, so the CPU crop encoders and the GPU kernels
address the same physical pages with nothing copied between them.

## Tiling

640 px tiles at native 10 m/px, 64 px overlap, edge tiles clamped inward so
every tile is full imagery and Ultralytics' 114-value letterbox padding never
comes into play.

Duplicates are handled twice over: detections whose centre falls in a tile's
overlap margin are dropped at decode time (the neighbouring tile sees the same
vessel more centrally), and a global rotated-NMS pass runs across the whole
scene afterwards.

## Land masking

All six GSHHG levels are unioned into one "not open sea" region and buffered
outward by `--buffer-m` (default 100). A detection is dropped when its **centre**
falls inside, tested against exact vector geometry — GEOS prepared geometries,
not raster quantisation.

Two things worth knowing:

**100 m will not suppress moored vessels in a real port.** GSHHG maps no
breakwaters, quays or moles, so a harbour basin is open water at every
resolution. Constanța sits inside this scene and berthed ships there will
survive a 100 m buffer comfortably. If dropping them matters, expect to need
500 m–1 km, or a separate port-polygon layer. Look at the debug JPEG over the
port before deciding.

**Lakes and ponds count as land here**, since all levels are unioned. Vessels on
the Danube delta lagoons and on the river itself are dropped.

**Tiles lying entirely inside the buffered exclusion zone are always skipped
before inference.** This is an accelerator, not an approximation: a detection
whose centre is at sea cannot come from a tile containing no sea, and centres on
land are dropped by the post-filter regardless. The coarse raster used for the
test is eroded by one cell first, so the test is conservative.

## Output

```json
{
  "scene": "S1C_IW_GRDH_1SDV_20260807T041243_...tif",
  "acquisition_time": "2026-08-07T04:12:43Z",
  "detections": [
    {
      "time": "2026-08-07T04:12:43Z",
      "confidence": 0.87,
      "center": {"lon": 29.1, "lat": 44.2},
      "heading_deg": 37.4,
      "length_m": 180.0, "width_m": 25.0,
      "corners_lonlat": [[...], [...], [...], [...]],
      "corners_pixel":  [[...], [...], [...], [...]]
    }
  ]
}
```

`time` is scene-level: the sensing **start** from the filename, identical for
every detection. `sensing_start` and `sensing_stop` are both in the header if
you later want to interpolate along azimuth.

`heading_deg` is the long-axis orientation as a true-north bearing folded to
0–180. It is computed by projecting two points to WGS84 and taking the geodetic
azimuth, so UTM grid convergence is handled without a convergence formula. The
range is 0–180 rather than 0–360 because an oriented box carries no bow/stern
information — this is an axis, not a course.

## Water crops

`--out-crops DIR` partitions the scene into non-overlapping `--crop-size`
windows (1024 px default, 10.24 km at 10 m/px) and writes one **grayscale** JPEG
per window that holds water, plus a `manifest.json` and a `thumbs/` tier. Land-
only and out-of-swath windows are never written, so what leaves the spacecraft
is the water and nothing else.

Grayscale, not RGB: the imagery is grey, and an RGB JPEG of a grey image spends
bytes on two chroma planes carrying nothing. No boxes are burnt in either — the
manifest carries the detection indices that fall inside each crop, so the ground
overlays them and the pixels stay clean for anything else you want to run.

### The shoreline

A window whose land fraction is above `--crop-coast-lo` is *coastal*. For each
one, the planner slides a window within ±size/2 and keeps the placement whose
land fraction is closest to 0.5 — the coast in context, rather than clipped off
wherever a grid line happened to fall. Measured on a synthetic coastline, those
windows land within 0.006 of exactly 50/50 on average.

The grid window underneath is kept **as well**, unless the shifted one already
contains all of its sea. That is what keeps the water coverage complete: shifting
a window landward vacates a strip of sea on its seaward side, and nothing else in
a non-overlapping grid would cover it. Coastal areas therefore carry some
overlap. That is the price of the guarantee.

`--crop-min-sea` is the one dial that relaxes the guarantee: it is the fraction
of a window's sea that is allowed to go uncovered, and it defaults to **0**,
meaning no water is ever dropped. Raise it if you would rather not send a crop
that is 95% land for the sake of one sliver of water in the corner.

Both filters run off summed-area tables over the decimated land and data masks,
so testing a candidate placement is three integer loads regardless of window
size, and planning a whole scene is sub-millisecond.

### Where it runs

Planning needs only the two masks, and extraction only reads the quantised
scene — neither waits on a detection. So the whole thing is dispatched onto CPU
worker threads immediately after triage and runs *underneath* the GPU inference
stage, on cores that would otherwise be blocked waiting for it. Only the
manifest, which needs the detection list, is written afterwards.

### Choosing `--crop-size`

Measured on a synthetic scene with a long wavy coastline, at three sizes:

| size | crops | payload | sea | land carried |
|-----:|------:|--------:|----:|-------------:|
| 512  | 278   | 73 Mpx  | 91.0% | 8.3% |
| 1024 | 79    | 83 Mpx  | 85.1% | 13.5% |
| 2048 | 25    | 105 Mpx | 74.4% | 24.6% |

Bigger windows fit the coastline worse and carry more land you did not ask for;
JPEG header overhead is ~600 bytes per file and never the deciding factor. 1024
is the default as the balance point: 14% more bytes than 512 for a quarter of
the file count. Drop to 512 if the link is the binding constraint, go to 2048
only if the per-file request count is.

## Overview render

`--out-overview PATH` box-filters the quantised scene down to `--overview-max`
(4096 px default) and draws the detections on it. A few hundred kB, a fraction
of a second, and it is what a human actually wants for situational awareness.

It is a box average rather than point sampling on purpose: at 8:1, nearest
neighbour drops most vessels between samples, which defeats the point.

Prefer this to `--out-jpg`. The full-resolution render is ~10 s, produces a file
most viewers cannot decode (640 Mpx), and nothing downstream reads it.

## Known gaps

- The channel layout `[cx, cy, w, h, score, angle]` is inferred from the export
  metadata, not verified by running the model. `Engine::introspect` hard-fails if
  the output does not have 6 channels, but it cannot catch a *reordering*. Run
  the onnxruntime probe in `01_inspect_onnx.py` to confirm before trusting output.
- No validation set for the detector itself, so detection quality is assessed
  only by eye from the debug JPEG.
- Engine build happens on first run if no plan is cached. Do it offline.
- The crop planner was verified against synthetic coastlines (wavy coast, island,
  thin sea sliver, all-land, open ocean) at 512/1024/2048 px — coverage,
  in-bounds windows, the 50/50 property and the encoder output all checked. It
  has **not** yet been run against a real GSHHG-derived mask on a real scene.
- Crop coverage is complete in terms of the *decimated* mask, i.e. to
  `--coarse-decim` granularity (80 m at the default 8). Sub-cell slivers of
  water are not tracked at all, by either the crops or the land filter.
- Multi-stream inference (`--streams`, default 2) can occasionally produce a
  different NMS outcome for two independently-run scenes when a ship sits
  exactly on a tile boundary and two near-duplicate raw detections are close
  enough in confidence that stream-completion timing decides which one
  survives. Observed on real data as a single detection's position/size
  differing by a few metres between repeated runs of the same scene with the
  same flags; the detection set as a whole was not affected. Not yet
  root-caused or fixed.
