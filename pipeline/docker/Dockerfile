# syntax=docker/dockerfile:1
#
# Build context must be pipeline/, e.g. from the pipeline/ directory:
#   docker build -f docker/Dockerfile -t sar-ship-detect .
#
# ---- Base image -------------------------------------------------------------
# MUST match this device's L4T/JetPack exactly: R39.2 (nvidia-l4t-core
# 39.2.0), JetPack 7.2-b187, CUDA 13.2, TensorRT 10.16.2 -- confirmed via
# `cat /etc/nv_tegra_release` and `dpkg -l | grep nvidia-l4t-core` on this
# box. Jetson containers share the host kernel GPU driver rather than
# virtualizing it, and engine.cpp's cached .engine is tied to the exact
# GPU + TensorRT build, so a mismatched base image either won't run or will
# silently rebuild the engine every start.
#
# VERIFY before building -- do not assume this tag exists:
#   https://catalog.ngc.nvidia.com/orgs/nvidia/containers/l4t-jetpack
ARG BASE_IMAGE=nvcr.io/nvidia/l4t-jetpack:r39.2.0

# ---- build stage --------------------------------------------------------
FROM ${BASE_IMAGE} AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build libgdal-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY *.cpp *.hpp *.cu *.cuh ./
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j

# ---- runtime stage --------------------------------------------------------
FROM ${BASE_IMAGE} AS runtime
# Package name depends on the GDAL version baked into this JetPack base --
# if this fails, run `apt list --installed | grep gdal` in the build stage
# and use whatever runtime package name shows up there instead.
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgdal32 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/sar_ship_detect /app/sar_ship_detect
COPY docker/entrypoint.sh /app/entrypoint.sh
RUN chmod +x /app/entrypoint.sh

# Single /data mount, self-contained under the host's yolov8s-obb-cpp/data/:
#   docker run --runtime nvidia -v $(pwd)/data:/data sar-ship-detect
ENV INPUT_DIR=/data/input \
    OUTPUT_DIR=/data/output \
    MODEL_DIR=/data/model \
    GSHHG_DIR=/data/GSHHS_shp/f \
    POLL_SECONDS=10 \
    DEBUG_JPG=0

VOLUME ["/data"]
ENTRYPOINT ["/app/entrypoint.sh"]
