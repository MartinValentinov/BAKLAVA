# SAR Ship Detection (YOLOv8s-OBB, Jetson Orin)

Sentinel-1 SAR GeoTIFF in → oriented ship detections (WGS84 coordinates,
heading, size) out, running on Jetson AGX Orin via TensorRT. See
**[MANUAL.md](MANUAL.md)** for full usage instructions and
**[pipeline/README.md](pipeline/README.md)** for the design rationale
(radiometry, tiling, land-masking math).

## Requirements

**Hardware:** NVIDIA Jetson AGX Orin (SM 8.7). The TensorRT engine this
pipeline builds is locked to the exact GPU + TensorRT version it was built
on — it will not run unmodified on other GPU architectures without
rebuilding, and won't run at all on non-NVIDIA hardware.

**Software** (versions this repo was built/tested against — see
`cat /etc/nv_tegra_release` and `dpkg -l | grep nvidia-l4t-core` to check
yours):

| Component | Version used here |
|---|---|
| L4T / JetPack | R39.2 / 7.2-b187 |
| CUDA | 13.2 |
| TensorRT | 10.16.2 |
| GDAL | 3.8.4 |
| CMake | ≥ 3.22 |

Build-time packages: `cmake`, `ninja-build`, `libgdal-dev`, `pkg-config`.
CUDA and TensorRT come from the JetPack SDK, not apt. **If you have a conda
environment active, deactivate it before building** — conda's GDAL is built
against a different GEOS/libstdc++ ABI, and the land-mask code will link
against the wrong one silently (see `pipeline/CMakeLists.txt`).

**Data, not included in this repo** (see below for where to get each):
- A Sentinel-1 GRD scene (float32 sigma0, VH, terrain-corrected) to run against.
- Trained model weights (`model_b16_640.onnx` — a shape-frozen YOLOv8s-OBB
  export, batch 16 / imgsz 640).
- GSHHG coastline shapefiles, full-resolution set (`GSHHS_f_L1..L6.shp`).

**Optional:** Docker + `nvidia-container-runtime` if running as the
containerized service (see [MANUAL.md §7](MANUAL.md#7-the-docker-service)).

## Getting the data

```
data/
├── input/         # put scene .tif files here
├── output/         # detections land here
├── model/          # put model_b16_640.onnx (+ cached .engine) here
└── GSHHS_shp/f/    # put GSHHS_f_L1..L6.shp (+ .dbf/.shx/.prj) here
```

- **GSHHG shapefiles**: public domain, from the Global Self-consistent
  Hierarchical High-resolution Geography project
  (https://www.soest.hawaii.edu/pwessel/gshhg/), distributed under the LGPL —
  see `data/GSHHS_shp/README.TXT` once downloaded. Only the full-resolution
  (`f`) set is used by this project; verify that URL is current before
  relying on it, data hosting locations can move over time.
- **Model weights**: not distributed in this repo (see `data/model/*.onnx`
  in `.gitattributes` — tracked via Git LFS if you do commit one). Source
  them from wherever your training run/model registry keeps them, or export
  your own and freeze its shape (dynamic ONNX exports won't build a TensorRT
  engine without a profile — see `pipeline/README.md`).
- **Scenes**: source your own Sentinel-1 GRD products; the sample used
  throughout `MANUAL.md` is not included (2.5GB, and not something to check
  into git regardless).

## Quick start

```bash
git clone <this-repo-url>
cd yolov8s-obb-cpp
git lfs pull                      # fetches the GSHHG shapefiles if you committed them

cd pipeline
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
cd ..

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
  -v
```

First run builds the TensorRT engine (~10+ minutes, one-time, GPU/TensorRT-
version-locked). Full flag reference, output schema, performance notes, and
the Docker service are all in **[MANUAL.md](MANUAL.md)**.

## Repository layout

```
yolov8s-obb-cpp/
├── pipeline/           C++/CUDA source (builds to sar_ship_detect)
│   ├── docker/         Dockerfile + entrypoint.sh for the containerized service
│   └── README.md       design rationale
├── data/               runtime data (mostly gitignored — see .gitignore)
├── MANUAL.md            full usage manual
├── README.md            this file
└── LICENSE
```

## License

Code in this repository (everything under `pipeline/`) is MIT-licensed —
see [LICENSE](LICENSE). GSHHG shapefile data, if you place it under
`data/GSHHS_shp/`, carries its own LGPL license from the GSHHG project, not
this repo's MIT terms. Model weights are not part of this repo and carry
whatever license your own training/export process assigns them.

## Known gaps

Tracked in detail in `MANUAL.md` §8 and `pipeline/README.md`. Headline ones:
no validation set (detection quality assessed by eye only), output channel
order assumed but not verified against a live model run, and the Docker
image has not yet been built/run end-to-end (only the entrypoint script's
logic has been tested directly against the compiled binary).
