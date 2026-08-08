#include "config.hpp"
#include "detection.hpp"
#include "engine.hpp"
#include "kernels.cuh"
#include "landmask.hpp"
#include "render.hpp"
#include "scene.hpp"
#include "util.hpp"

#include <gdal_priv.h>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <future>
#include <numeric>
#include <vector>

namespace {

struct StreamCtx {
    cudaStream_t stream{};
    float* dIn   = nullptr;      // NCHW batch
    float* dOut  = nullptr;      // TRT output
    int2*  dOrig = nullptr;      // tile origins for this batch
};

// ---- host-side sequential sweep over the GPU-computed IoU bitmask -----------
// The O(N^2) rotated-IoU work is on the GPU; only this inherently serial sweep
// is on the CPU, and at a few thousand boxes it is microseconds.
std::vector<int> nmsSweep(const std::vector<unsigned long long>& mask,
                          int n, int colBlocks) {
    std::vector<unsigned long long> removed(colBlocks, 0ULL);
    std::vector<int> keep;
    keep.reserve(n);
    for (int i = 0; i < n; ++i) {
        const int blk = i / 64, bit = i % 64;
        if (removed[blk] & (1ULL << bit)) continue;
        keep.push_back(i);
        const unsigned long long* p = &mask[size_t(i) * colBlocks];
        for (int j = blk; j < colBlocks; ++j) removed[j] |= p[j];
    }
    return keep;
}

std::string jsonEscape(const std::string& s) {
    std::string o;
    for (char ch : s) {
        if (ch == '"' || ch == '\\') { o += '\\'; o += ch; }
        else if (ch == '\n') o += "\\n";
        else o += ch;
    }
    return o;
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg = Config::parse(argc, argv);
    Timer tTotal("TOTAL");

    GDALAllRegister();

    // ---------------------------------------------------------------- stage 1
    // Scene read, land mask and engine load are independent. The read is the
    // slow one and is explicitly outside the latency target, so overlap the
    // other two with it.
    Scene scene;
    if (!scene.open(cfg.tif)) return 1;

    auto fLand = std::async(std::launch::async, [&]() {
        Timer t("landmask build");
        LandMask* lm = new LandMask();
        lm->build(cfg.gshhg, scene, cfg.buffer_m, cfg.coarse_decim);
        t.report();
        return lm;
    });

    auto fEngine = std::async(std::launch::async, [&]() {
        Timer t("engine load");
        Engine* e = new Engine();
        bool ok = e->loadOrBuild(cfg.onnx, cfg.engine, cfg.fp16, cfg.workspace_mb);
        t.report();
        if (!ok) { delete e; return (Engine*)nullptr; }
        return e;
    });

    {
        Timer t("scene read (excluded)");
        if (!scene.readAll()) return 1;
        t.report();
    }

    std::unique_ptr<LandMask> land(fLand.get());
    std::unique_ptr<Engine> eng(fEngine.get());
    if (!eng) return 1;

    // From here on the clock is the one that matters.
    Timer tHot("HOT PATH (post-load)");

    const int W = scene.width(), H = scene.height();
    const int T = eng->imgsz();
    const int B = eng->batch();
    const int A = eng->anchors();

    if (T != cfg.imgsz)
        std::fprintf(stderr, "[warn] engine imgsz %d overrides --imgsz %d\n", T, cfg.imgsz);
    if (W < T || H < T) FATAL("scene smaller than one tile (%dx%d < %d)", W, H, T);

    // ---------------------------------------------------------------- stage 2
    // Tile grid. Edge tiles are clamped inward rather than padded, so every
    // tile is a full 640x640 of real imagery and the 114-pad case never arises.
    const int stride = T - cfg.overlap;
    const int nx = (W - T + stride - 1) / stride + 1;
    const int ny = (H - T + stride - 1) / stride + 1;

    std::vector<int2> tiles;
    tiles.reserve(size_t(nx) * ny);
    for (int j = 0; j < ny; ++j) {
        for (int i = 0; i < nx; ++i) {
            int2 o;
            o.x = std::min(i * stride, W - T);
            o.y = std::min(j * stride, H - T);
            tiles.push_back(o);
        }
    }
    const size_t nTilesRaw = tiles.size();

    // ---------------------------------------------------------------- stage 3
    // Triage. Two filters, both provably output-preserving:
    //   * all-zero tiles are entirely out of swath -- no imagery, no vessels
    //   * fully-land tiles cannot hold a detection whose centre is at sea, and
    //     centres on land are dropped by the post-filter anyway
    // Disable the second with --no-skip-land-tiles to see land detections in
    // the debug render.
    {
        Timer t("tile triage");
        const int bs = 64;
        const int bw = (W + bs - 1) / bs, bh = (H + bs - 1) / bs;

        float* dBlockMax = nullptr;
        CUDA_CHECK(cudaMalloc(&dBlockMax, size_t(bw) * bh * sizeof(float)));
        launch_block_max(scene.device(), W, H, bs, bw, bh, dBlockMax, 0);
        CUDA_CHECK(cudaDeviceSynchronize());

        std::vector<float> blockMax(size_t(bw) * bh);
        CUDA_CHECK(cudaMemcpy(blockMax.data(), dBlockMax,
                              blockMax.size() * sizeof(float), cudaMemcpyDeviceToHost));
        CUDA_CHECK(cudaFree(dBlockMax));

        std::vector<int2> kept;
        kept.reserve(tiles.size());
        size_t droppedNodata = 0, droppedLand = 0;

        for (const int2& o : tiles) {
            if (cfg.skip_nodata_tiles) {
                float m = 0.0f;
                const int b0 = o.x / bs, b1 = (o.x + T - 1) / bs;
                const int r0 = o.y / bs, r1 = (o.y + T - 1) / bs;
                for (int r = r0; r <= r1 && m == 0.0f; ++r)
                    for (int b = b0; b <= b1; ++b)
                        m = std::max(m, blockMax[size_t(r) * bw + b]);
                if (m <= 0.0f) { ++droppedNodata; continue; }
            }
            if (cfg.skip_land_tiles && land->ready() && land->tileFullyLand(o.x, o.y, T)) {
                ++droppedLand;
                continue;
            }
            kept.push_back(o);
        }
        tiles.swap(kept);
        t.report();
        std::fprintf(stderr, "[tiles] %zu total -> %zu inferred "
                             "(%zu out-of-swath, %zu fully land)\n",
                     nTilesRaw, tiles.size(), droppedNodata, droppedLand);
    }

    if (tiles.empty()) {
        std::fprintf(stderr, "[tiles] nothing to infer\n");
    }

    // ---------------------------------------------------------------- stage 4
    // Inference. One CUDA stream and one TRT context per lane; lanes run
    // preprocess -> enqueue -> decode back to back so the GPU never idles
    // between a batch's stages.
    const int lanes = std::max(1, cfg.streams);
    if (!eng->createContexts(lanes)) FATAL("could not create %d TRT contexts", lanes);

    std::vector<StreamCtx> lane(lanes);
    for (int i = 0; i < lanes; ++i) {
        CUDA_CHECK(cudaStreamCreateWithFlags(&lane[i].stream, cudaStreamNonBlocking));
        CUDA_CHECK(cudaMalloc(&lane[i].dIn,  eng->inputElems()  * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&lane[i].dOut, eng->outputElems() * sizeof(float)));
        CUDA_CHECK(cudaMalloc(&lane[i].dOrig, size_t(B) * sizeof(int2)));
    }

    Det* dDets = nullptr;
    int* dCount = nullptr;
    CUDA_CHECK(cudaMalloc(&dDets, size_t(cfg.max_det) * sizeof(Det)));
    CUDA_CHECK(cudaMalloc(&dCount, sizeof(int)));
    CUDA_CHECK(cudaMemset(dCount, 0, sizeof(int)));

    {
        Timer t("inference");
        const size_t nBatches = (tiles.size() + B - 1) / B;
        for (size_t bi = 0; bi < nBatches; ++bi) {
            StreamCtx& L = lane[bi % lanes];
            const size_t off = bi * B;
            const int n = int(std::min<size_t>(B, tiles.size() - off));

            // Short batches are padded by repeating the last origin: the engine
            // has a fixed batch dimension, and duplicate tiles only ever produce
            // duplicate detections, which NMS collapses.
            std::vector<int2> batch(B, tiles[off + n - 1]);
            std::copy(tiles.begin() + off, tiles.begin() + off + n, batch.begin());

            CUDA_CHECK(cudaMemcpyAsync(L.dOrig, batch.data(), size_t(B) * sizeof(int2),
                                       cudaMemcpyHostToDevice, L.stream));

            launch_preprocess(scene.device(), W, H, L.dOrig, B, T,
                              cfg.db_lo, cfg.db_hi, L.dIn, L.stream);

            if (!eng->enqueue(int(bi % lanes), L.dIn, L.dOut, L.stream))
                FATAL("enqueueV3 failed on batch %zu", bi);

            // Only decode the real tiles of a short final batch.
            launch_decode(L.dOut, n, A, L.dOrig, cfg.conf, T, cfg.overlap,
                          W, H, dDets, dCount, cfg.max_det, L.stream);
        }
        for (int i = 0; i < lanes; ++i) CUDA_CHECK(cudaStreamSynchronize(lane[i].stream));
        t.report();
        std::fprintf(stderr, "[infer] %zu batches of %d over %d lanes\n",
                     nBatches, B, lanes);
    }

    int nRaw = 0;
    CUDA_CHECK(cudaMemcpy(&nRaw, dCount, sizeof(int), cudaMemcpyDeviceToHost));
    if (nRaw > cfg.max_det) {
        std::fprintf(stderr, "[warn] %d raw detections exceeded --max-det %d; truncated\n",
                     nRaw, cfg.max_det);
        nRaw = cfg.max_det;
    }
    std::fprintf(stderr, "[detect] %d raw above conf %.2f\n", nRaw, cfg.conf);

    // ---------------------------------------------------------------- stage 5
    // Global rotated NMS across the whole scene, so vessels straddling a tile
    // boundary are merged rather than reported twice.
    std::vector<Det> dets(nRaw);
    if (nRaw > 0) {
        Timer t("rotated NMS");
        CUDA_CHECK(cudaMemcpy(dets.data(), dDets, size_t(nRaw) * sizeof(Det),
                              cudaMemcpyDeviceToHost));
        std::sort(dets.begin(), dets.end(),
                  [](const Det& a, const Det& b) { return a.score > b.score; });
        CUDA_CHECK(cudaMemcpy(dDets, dets.data(), size_t(nRaw) * sizeof(Det),
                              cudaMemcpyHostToDevice));

        const int colBlocks = (nRaw + 63) / 64;
        unsigned long long* dMask = nullptr;
        CUDA_CHECK(cudaMalloc(&dMask, size_t(nRaw) * colBlocks * sizeof(unsigned long long)));
        CUDA_CHECK(cudaMemset(dMask, 0, size_t(nRaw) * colBlocks * sizeof(unsigned long long)));
        launch_rotated_nms_mask(dDets, nRaw, cfg.nms_iou, dMask, colBlocks, 0);
        CUDA_CHECK(cudaDeviceSynchronize());

        std::vector<unsigned long long> mask(size_t(nRaw) * colBlocks);
        CUDA_CHECK(cudaMemcpy(mask.data(), dMask, mask.size() * sizeof(unsigned long long),
                              cudaMemcpyDeviceToHost));
        CUDA_CHECK(cudaFree(dMask));

        std::vector<int> keep = nmsSweep(mask, nRaw, colBlocks);
        std::vector<Det> merged;
        merged.reserve(keep.size());
        for (int i : keep) merged.push_back(dets[i]);
        dets.swap(merged);
        t.report();
        std::fprintf(stderr, "[nms] %d -> %zu\n", nRaw, dets.size());
    }

    // ---------------------------------------------------------------- stage 6
    // Land filter on detection centres, using exact vector geometry.
    std::vector<Det> sea;
    {
        Timer t("land filter");
        size_t dropped = 0;
        for (const Det& d : dets) {
            if (land->ready() && land->polygonCount() > 0) {
                double x, y;
                scene.pixelToMap(d.cx, d.cy, x, y);
                if (land->isLand(x, y)) { ++dropped; continue; }
            }
            sea.push_back(d);
        }
        t.report();
        std::fprintf(stderr, "[land] dropped %zu on land/within %.0f m of shore, %zu remain\n",
                     dropped, cfg.buffer_m, sea.size());
    }

    // ---------------------------------------------------------------- stage 7
    // Geo conversion.
    std::vector<GeoDet> out;
    out.reserve(sea.size());
    for (const Det& d : sea) {
        GeoDet g{};
        const double ca = std::cos(d.angle), sa = std::sin(d.angle);
        const double hw = d.w * 0.5, hh = d.h * 0.5;
        const double ox[4] = {-hw,  hw, hw, -hw};
        const double oy[4] = {-hh, -hh, hh,  hh};
        for (int i = 0; i < 4; ++i) {
            g.px[i] = d.cx + ox[i] * ca - oy[i] * sa;
            g.py[i] = d.cy + ox[i] * sa + oy[i] * ca;
            scene.pixelToLonLat(g.px[i], g.py[i], g.lon[i], g.lat[i]);
        }
        scene.pixelToLonLat(d.cx, d.cy, g.center_lon, g.center_lat);

        const double px = std::fabs(scene.pixelSizeX());
        g.length_m = std::max(d.w, d.h) * px;
        g.width_m  = std::min(d.w, d.h) * px;

        // Long axis in pixel space, then a true-north bearing folded to 0-180.
        // An OBB carries no bow/stern information, so 0-180 is the honest range:
        // this is an axis orientation, not a direction of travel.
        const double ux = (d.w >= d.h) ? ca : -sa;
        const double uy = (d.w >= d.h) ? sa :  ca;
        double bearing = 0.0;
        scene.pixelDirToBearing(d.cx, d.cy, ux, uy, bearing);
        g.heading_deg = std::fmod(std::fmod(bearing, 180.0) + 180.0, 180.0);

        g.score = d.score;
        out.push_back(g);
    }

    // ---------------------------------------------------------------- stage 8
    {
        std::ofstream f(cfg.out_json);
        if (!f) FATAL("cannot write %s", cfg.out_json.c_str());
        f.precision(9);
        f << std::fixed;
        f << "{\n";
        f << "  \"scene\": \"" << jsonEscape(scene.basename()) << "\",\n";
        f << "  \"acquisition_time\": \"" << scene.acquisitionISO() << "\",\n";
        f << "  \"sensing_start\": \"" << scene.startISO() << "\",\n";
        f << "  \"sensing_stop\": \"" << scene.stopISO() << "\",\n";
        f << "  \"detection_count\": " << out.size() << ",\n";
        f << "  \"detections\": [\n";
        for (size_t i = 0; i < out.size(); ++i) {
            const GeoDet& g = out[i];
            f << "    {\n";
            f << "      \"time\": \"" << scene.acquisitionISO() << "\",\n";
            f << "      \"confidence\": " << g.score << ",\n";
            f << "      \"center\": {\"lon\": " << g.center_lon
              << ", \"lat\": " << g.center_lat << "},\n";
            f << "      \"heading_deg\": " << g.heading_deg << ",\n";
            f << "      \"length_m\": " << g.length_m
              << ", \"width_m\": " << g.width_m << ",\n";
            f << "      \"corners_lonlat\": [";
            for (int k = 0; k < 4; ++k)
                f << "[" << g.lon[k] << ", " << g.lat[k] << "]" << (k < 3 ? ", " : "");
            f << "],\n";
            f << "      \"corners_pixel\": [";
            for (int k = 0; k < 4; ++k)
                f << "[" << g.px[k] << ", " << g.py[k] << "]" << (k < 3 ? ", " : "");
            f << "]\n";
            f << "    }" << (i + 1 < out.size() ? "," : "") << "\n";
        }
        f << "  ]\n}\n";
        std::fprintf(stderr, "[out] wrote %s (%zu detections)\n",
                     cfg.out_json.c_str(), out.size());
    }

    tHot.report();

    // ---------------------------------------------------------------- stage 9
    // Debug render, deliberately after the metadata is on disk: it is the most
    // expensive stage and nothing downstream waits on it.
    if (!cfg.out_jpg.empty()) {
        Timer t("debug render");
        uint8_t* dRGB = nullptr;
        const size_t rgbBytes = size_t(W) * H * 3;
        if (cudaMalloc(&dRGB, rgbBytes) != cudaSuccess) {
            std::fprintf(stderr, "[jpeg] cannot allocate %.2f GB for the render; skipping\n",
                         rgbBytes / 1e9);
        } else {
            launch_render_rgb(scene.device(), W, H, cfg.db_lo, cfg.db_hi, dRGB, 0);

            Det* dSea = nullptr;
            if (!sea.empty()) {
                CUDA_CHECK(cudaMalloc(&dSea, sea.size() * sizeof(Det)));
                CUDA_CHECK(cudaMemcpy(dSea, sea.data(), sea.size() * sizeof(Det),
                                      cudaMemcpyHostToDevice));
                launch_draw_boxes(dRGB, W, H, dSea, int(sea.size()),
                                  cfg.box_thickness, 0);
            }
            CUDA_CHECK(cudaDeviceSynchronize());
            encodeJpegRGB(dRGB, W, H, cfg.jpeg_quality, cfg.out_jpg, 0);
            if (dSea) CUDA_CHECK(cudaFree(dSea));
            CUDA_CHECK(cudaFree(dRGB));
        }
        t.report();
    }

    for (int i = 0; i < lanes; ++i) {
        CUDA_CHECK(cudaFree(lane[i].dIn));
        CUDA_CHECK(cudaFree(lane[i].dOut));
        CUDA_CHECK(cudaFree(lane[i].dOrig));
        CUDA_CHECK(cudaStreamDestroy(lane[i].stream));
    }
    CUDA_CHECK(cudaFree(dDets));
    CUDA_CHECK(cudaFree(dCount));

    tTotal.report();
    return 0;
}
