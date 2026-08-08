# SAR ship detection — YOLOv8s-OBB on Jetson AGX Orin

Sentinel-1 GRD (float32 sigma0, VH, terrain-corrected and denoised) → oriented
vessel detections with WGS84 coordinates, plus an optional full-resolution
debug render.

Target: JetPack 7.2 / L4T R39.2, CUDA 13.2, TensorRT 10.16.2, SM 8.7.

## Build

```bash
sudo apt install -y cmake ninja-build libgdal-dev      # cmake and ninja were missing
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
  --out-json detections.json \
  --out-jpg  debug.jpg
```

Every stage prints its wall time to stderr. `HOT PATH (post-load)` is the number
to watch — it excludes the GeoTIFF read, per the agreed budget.

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
falls inside, tested against exact vector geometry — no raster quantisation.

Two things worth knowing:

**100 m will not suppress moored vessels in a real port.** GSHHG maps no
breakwaters, quays or moles, so a harbour basin is open water at every
resolution. Constanța sits inside this scene and berthed ships there will
survive a 100 m buffer comfortably. If dropping them matters, expect to need
500 m–1 km, or a separate port-polygon layer. Look at the debug JPEG over the
port before deciding.

**Lakes and ponds count as land here**, since all levels are unioned. Vessels on
the Danube delta lagoons and on the river itself are dropped.

### `--no-skip-land-tiles`

By default, tiles lying entirely inside the buffered exclusion zone are skipped
before inference. This is an accelerator, not an approximation: a detection
whose centre is at sea cannot come from a tile containing no sea, and centres on
land are dropped by the post-filter regardless. The coarse raster used for the
test is eroded by one cell first, so the test is conservative.

Pass `--no-skip-land-tiles` to run everything — useful when you want to *see*
the land false positives in the debug render.

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

## Known gaps

- The channel layout `[cx, cy, w, h, score, angle]` is inferred from the export
  metadata, not verified by running the model. `Engine::introspect` hard-fails if
  the output does not have 6 channels, but it cannot catch a *reordering*. Run
  the onnxruntime probe in `01_inspect_onnx.py` to confirm before trusting output.
- No validation set, so detection quality is assessed only by eye from the debug
  JPEG.
- Engine build happens on first run if no plan is cached. Do it offline.
