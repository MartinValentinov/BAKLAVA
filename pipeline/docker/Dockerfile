ARG BASE_IMAGE=nvcr.io/nvidia/pytorch:26.03-py3

FROM ${BASE_IMAGE} AS build
RUN apt-get update && apt-get install -y --no-install-recommends \
        cmake ninja-build libgdal-dev libjpeg-dev pkg-config \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /src
COPY CMakeLists.txt ./
COPY main.cpp config.hpp config.cpp ./
COPY common/ ./common/
COPY io/ ./io/
COPY l1/ ./l1/
COPY detect/ ./detect/
COPY output/ ./output/
RUN cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release \
    && cmake --build build -j

FROM ${BASE_IMAGE} AS runtime
RUN apt-get update && apt-get install -y --no-install-recommends \
        libgdal34t64 \
    && rm -rf /var/lib/apt/lists/*

COPY --from=build /src/build/sar_ship_detect /app/sar_ship_detect
COPY docker/entrypoint.sh /app/entrypoint.sh
COPY docker/serve_supervisor.sh /app/serve_supervisor.sh
RUN chmod +x /app/entrypoint.sh /app/serve_supervisor.sh

ENV INPUT_DIR=/data/input \
    OUTPUT_DIR=/data/output \
    MODEL_DIR=/data/model \
    GSHHG_DIR=/data/GSHHS_shp/f \
    POLL_SECONDS=10 \
    DEBUG_JPG=0

VOLUME ["/data"]
ENTRYPOINT ["/app/entrypoint.sh"]
