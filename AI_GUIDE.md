# forsee detector — what everything is, and why

A map of this folder, aimed at someone who wants to understand the thing
rather than look up an API. `MANUAL.md` is the operational reference (flags,
timings, deployment); this explains what the parts *are*.

The short version: this program turns a Sentinel-1 radar image into a list of
ship positions. The neural network is a small part of it. Most of the code
exists because radar imagery is not a photograph — it arrives as raw signal
data with no map coordinates, and the physics of turning it into georeferenced
pixels is most of the work.

---

## 1. The journey of one scene

```
 a .SAFE folder (raw Sentinel-1)          a .tif (already processed)
            │                                        │
            ▼                                        ▼
   safe.cpp   read the metadata            scene.cpp  load the raster
   rdgeom.cpp work out where the satellite was       │
   l1proc.cpp turn radar signal into map pixels      │
              k_rd_geocode writes 8-bit    kernels.cu  k_quantise:
              grey directly (fixed −25…0               dB → 8-bit grey
              dB) -- no float32 raster                 (same window)
            │                                        │
            └────────────► 8-bit grey ◄──────────────┘
                                 │
                   landmask.cpp  which pixels are land?
                                 │
                          main.cpp  cut into 640×640 tiles,
                                    throw away land and empty ones
                                 │
                    kernels.cu   k_preprocess: tiles → tensor
                                 │
                    engine.cpp   ***THE NEURAL NETWORK***
                                 │
                    kernels.cu   k_decode: raw numbers → boxes
                                 │
                    kernels.cu   k_nms_mask: remove duplicates
                                 │
                      main.cpp   pixel → longitude/latitude
                                 │
                     detections.json  +  optional JPEGs
```

The only part that is "AI" is one line in `main.cpp` calling `engine.cpp`.
Everything upstream exists to hand it a correctly rendered 640×640 tile;
everything downstream turns its output into coordinates on Earth.

---

## 2. Files, by what they're for

### The neural network

| file | what it does |
|---|---|
| `engine.cpp/.hpp` | Loads the ONNX model, builds a TensorRT engine (once, then cached to disk), runs inference. `introspect()` is the gatekeeper — it **refuses to load a model whose output isn't exactly 6 channels**, because the decode kernel assumes that layout. |
| `int8calib.cpp/.hpp` | INT8 quantisation. Feeds a few hundred real tiles through the network at build time so TensorRT can pick number ranges. Roughly 1.5–2× faster than FP16 on Orin. Optional — `--int8-data`. |
| `detection.hpp` | The two structs everything passes around. `Det` is a box in tile pixels; `GeoDet` is the same box in longitude/latitude with length, width, and a heading + confidence in degrees/0–1. |

### GPU work — `kernels.cu` / `kernels.cuh`

Two files, eleven kernels between them. This is where nearly all the compute happens.

| kernel | what it does |
|---|---|
| `k_calibrate` | raw radar counts → calibrated backscatter |
| `k_rd_geocode` | the hard one: for each output map pixel, work backwards to find which radar sample it came from (range–Doppler). On the `.SAFE` path it also **quantises as it writes** — grey plus the coarse triage grids straight out of the kernel, so nothing has to walk a float32 raster afterwards, and that raster is only allocated when `--out-l2` asks for it |
| `k_quantise` | dB → 8-bit grey, the fixed −25…0 dB window. Still the `.tif` path's own pass; the `.SAFE` path gets the same result from `k_rd_geocode`. Both call `sigma_to_u8` in `common/quantise.cuh` so the window cannot drift between them |
| `k_preprocess` | **cuts a 640×640 tile and writes it as the network's input tensor.** Copies pixels 1:1 — it never resizes. That single fact constrains how training data must be made (see §3) |
| `k_decode` | network output → boxes, drops anything below the confidence threshold, and discards boxes near a tile edge that another overlapping tile will see better |
| `k_nms_mask` | rotated non-maximum suppression — the same ship seen in two tiles becomes one detection |
| `k_estimate_heading` | one thread per surviving detection: samples the grayscale tile along the box's major/minor axes and picks which end is the bow (dimmer, more water-diluted = tapered) to turn the 0–180 axis into a 0–360 heading. Heuristic, not a measurement — see `pipeline/README.md` |
| `k_render_rgb`, `k_overview_rgb`, `k_draw_boxes`, `k_draw_arrows` | the optional debug pictures — the last one burns in the heading arrow next to each box |

### Getting from raw radar to an image

This is the part that isn't about ships at all.

| file | what it does |
|---|---|
| `safe.cpp/.hpp` | Reads a `.SAFE` folder: satellite orbit vectors, calibration tables, the slant-range→ground-range polynomials, ground control points. Pure XML/metadata parsing. |
| `rdgeom.cpp/.hpp` | The orbital geometry. Where was the satellite at time *t*, and for a point on the ground, at what instant was it exactly sideways from the radar (zero Doppler)? This is what makes pixels land in the right place. |
| `l1proc.cpp/.hpp` | Drives the above: reads the DEM (ground elevation) and geoid, then produces a calibrated, map-projected `VH_dB.tif`. Turns "raw satellite download" into "image you can look at". |
| `scene.cpp/.hpp` | Opens a GeoTIFF via GDAL, reads it (with a fast raw-strip path), and converts pixel ↔ map coordinates. |

### Land, output, plumbing

| file | what it does |
|---|---|
| `landmask.cpp/.hpp` | Loads GSHHG coastline shapefiles, buffers them by `--buffer-m`, and answers "is this land?". Used to skip whole tiles before inference — cheaper *and* removes false positives, since a building is not a boat. |
| `crops.cpp/.hpp` | `--out-crops`: writes the water areas as standalone JPEG chips plus a manifest. For review, not detection. |
| `render.cpp`, `jpeg.cpp` | JPEG encoding — GPU (nvJPEG) with a host libjpeg-turbo fallback. This is the **debug render**, and it is the single most expensive stage in the whole program (~10 s). Only runs if you ask for pictures. |
| `config.cpp/.hpp` | Every command-line flag and its default. Read this first when you want to know what's tunable. |
| `main.cpp` | Orchestrates all of it: tile the scene, triage, batch through the network across CUDA streams, NMS, convert to lon/lat, write `detections.json`. |
| `util.hpp` | `FATAL`, timers, small helpers. |
| `CMakeLists.txt`, `docker/`, `run.sh` | Build and run. `run.sh` is the entry point; it mounts `data/` into the container and picks the model by `ONNX_NAME`. |

---

## 3. The model contract — read before retraining

`engine.cpp` and `kernels.cu` make hard assumptions. Break one and you either
get a refusal to load, or — worse — a model that works in testing and is
quietly wrong in the field.

| rule | enforced where | why it matters |
|---|---|---|
| output exactly 6 channels `(cx, cy, w, h, score, angle)` | `engine.cpp` `introspect()` — returns false | **single class only.** A vessel/non-vessel two-class head changes the shape and `k_decode` would misread it |
| 640×640, no dynamic axes | `config.hpp`, `engine.cpp` build check | engine is frozen at build time |
| batch 16 | `run.sh` (`ONNX_NAME=model_b16_640.onnx`) | `main.cpp` takes batch from the engine, so anything loads — but 16 is what the pipeline is tuned for |
| input is grey/255 in 3 identical channels | `k_preprocess` | no colour, no ImageNet normalisation |
| fixed −25…0 dB render | `config.hpp` `db_lo`/`db_hi`, `common/quantise.cuh` `sigma_to_u8` | training data must use the same window, **not** a per-scene adaptive stretch. One definition, shared by `k_quantise` and `k_rd_geocode` — change it there or the two paths disagree |
| **tiles are copied 1:1, never resized** | `k_preprocess` | training tiles must be cut at native 640. Cutting 320 px crops and upscaling them to 640 trains fine and exports fine, then shows the deployed model every ship at half the scale it learned |

The last one is the trap. Nothing warns you.

### Swapping in a new model

1. Export ONNX at 640, batch 16, static shapes, single class.
2. Confirm it reports `[16,3,640,640] → [16,6,8400]`.
   (8400 = 80² + 40² + 20², the three detection scales.)
3. Drop it in `data/model/`, set `ONNX_NAME`.
4. **Delete the old `.engine` file** — a cached engine is reused as-is and your
   new ONNX will be ignored.

---

## 4. Knobs that actually change results

From `config.hpp`:

| flag | default | effect |
|---|---|---|
| `--conf` | 0.50 | the big one. Lower = finds more ships, more false alarms. Tune it against measured precision/recall, don't guess |
| `--overlap` | 64 px | tile overlap. More = better on ships near tile edges, more compute |
| `--buffer-m` | 100 | how far inland the land mask reaches |
| `--no-skip-land-tiles` | off | keeps land tiles. Slower, more false positives |
| `--int8-data` | unset | enables INT8. Rebuild the engine after setting it |
| `--out-jpg`, `--out-overview` | unset | the debug pictures — ~10 s. Leave off when you care about latency |

---

## 5. Where the training side lives

Not in this folder. Model training is in `custom/`:

- `scripts/pipeline/xview3_to_yolo.py` — xView3 scenes → 640 px training tiles
- `scripts/pipeline/train_xv3.py` — trains, then exports ONNX and checks it
  against the contract in §3
- `scripts/pipeline/score_xview3.py` — real accuracy, xView3's own metric
- `scripts/pipeline/make_int8_calib.py` — builds the INT8 calibration file
- `verify.sh` — one command, scores on held-out scenes
- `TRAINING.md` — why the training data is built the way it is

---

## 6. If you're lost, start here

- **"Where is the AI?"** → `engine.cpp` runs it, `k_preprocess` feeds it,
  `k_decode` reads it. Three places.
- **"Why is it slow?"** → `MANUAL.md` §timings. Usually the debug render.
- **"Why did it miss/invent a ship?"** → `--conf`, then the land mask, then the
  model. In that order.
- **"I retrained and it got worse in the field"** → §3, last row.
