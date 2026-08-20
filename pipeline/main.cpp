#include "config.hpp"
#include "common/detection.hpp"
#include "common/util.hpp"
#include "detect/engine.hpp"
#include "detect/kernels.cuh"
#include "io/landmask.hpp"
#include "io/scene.hpp"
#include "l1/l1proc.hpp"
#include "output/crops.hpp"
#include "output/jpeg.hpp"
#include "output/kernels.cuh"
#include "output/render.hpp"

#include <gdal_priv.h>

#include <fcntl.h>
#include <unistd.h>
#include <sstream>
#include <atomic>
#include <thread>
#include <sys/stat.h>
#include <stdexcept>
#include <cerrno>
#include <ctime>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <future>
#include <numeric>
#include <vector>

namespace {

struct StreamCtx {
    cudaStream_t stream{};
    float* dIn   = nullptr;
    float* dOut  = nullptr;
    int2*  dOrig = nullptr;
};

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

}

static int runOnce(const Config& cfg) {
    Timer tTotal("TOTAL");

    Scene scene;
    const bool fromL1 = !cfg.l1.safe.empty();
    L1Quant l1q;

    auto fEngine = std::async(std::launch::async, [&]() {
        Timer t("engine load");
        Engine* e = new Engine();
        bool ok = e->loadOrBuild(cfg.onnx, cfg.engine, true, cfg.workspace_mb);
        t.report();
        if (!ok) { delete e; return (Engine*)nullptr; }
        return e;
    });

    std::future<LandMask*> fLand;
    auto startLandmask = [&]() {
        fLand = std::async(std::launch::async, [&]() {
            Timer t("landmask build");
            LandMask* lm = new LandMask();
            lm->build(cfg.gshhg, scene, cfg.buffer_m, cfg.coarse_decim);
            t.report();
            return lm;
        });
    };

    if (fromL1) {

        l1q.enabled = true;

        l1q.hostVisible = !cfg.out_crops.empty();
        l1q.decim   = std::max(1, cfg.coarse_decim);
        l1q.db_lo   = cfg.db_lo;
        l1q.db_hi   = cfg.db_hi;
        if (!runL1ToL2(cfg.l1, scene, startLandmask, &l1q)) return 1;
    }

    if (!fromL1 && !scene.open(cfg.tif)) return 1;
    if (!fLand.valid()) startLandmask();

    if (!fromL1) {
        Timer t("scene read (excluded)");
        if (!scene.readAll()) return 1;
        t.report();
    }

    std::unique_ptr<LandMask> land(fLand.get());
    std::unique_ptr<Engine> eng(fEngine.get());
    if (!eng) return 1;

    Timer tHot("HOT PATH (post-load)");

    const int W = scene.width(), H = scene.height();
    const int T = eng->imgsz();
    const int B = eng->batch();
    const int A = eng->anchors();

    if (W < T || H < T) FATAL("scene smaller than one tile (%dx%d < %d)", W, H, T);

    const int D  = std::max(1, cfg.coarse_decim);
    const int cw = (W + D - 1) / D;
    const int ch = (H + D - 1) / D;

    uint8_t *grayHost = nullptr, *grayDev = nullptr;
    std::vector<uint8_t> dataMask;
    std::vector<unsigned int> maxGrid;
    std::vector<unsigned int> sumGrid;
    if (l1q.enabled) {

        if (l1q.cw != cw || l1q.ch != ch)
            FATAL("L1 coarse grid %dx%d does not match the detector's %dx%d",
                  l1q.cw, l1q.ch, cw, ch);
        grayHost = l1q.grayHost;
        grayDev  = l1q.grayDev;
        dataMask = std::move(l1q.dataMask);
        maxGrid  = std::move(l1q.maxGrid);
        sumGrid  = std::move(l1q.sumGrid);
    } else {
        Timer t("quantise 8-bit");
        dataMask.assign(size_t(cw) * ch, 0);
        maxGrid.assign(size_t(cw) * ch, 0);
        sumGrid.assign(size_t(cw) * ch, 0);
        if (cfg.out_crops.empty()) {
            CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&grayDev), size_t(W) * H));
        } else {
            CUDA_CHECK(cudaHostAlloc(reinterpret_cast<void**>(&grayHost), size_t(W) * H,
                                     cudaHostAllocMapped | cudaHostAllocPortable));
            CUDA_CHECK(cudaHostGetDevicePointer(reinterpret_cast<void**>(&grayDev), grayHost, 0));
        }

        uint8_t* dmDev = nullptr;
        CUDA_CHECK(cudaMalloc(&dmDev, dataMask.size()));
        CUDA_CHECK(cudaMemset(dmDev, 0, dataMask.size()));

        unsigned int* mxDev = nullptr;
        CUDA_CHECK(cudaMalloc(&mxDev, maxGrid.size() * sizeof(unsigned int)));
        CUDA_CHECK(cudaMemset(mxDev, 0, maxGrid.size() * sizeof(unsigned int)));

        unsigned int* smDev = nullptr;
        CUDA_CHECK(cudaMalloc(&smDev, sumGrid.size() * sizeof(unsigned int)));
        CUDA_CHECK(cudaMemset(smDev, 0, sumGrid.size() * sizeof(unsigned int)));

        launch_quantise(scene.device(), W, H, cfg.db_lo, cfg.db_hi, D,
                        grayDev, dmDev, mxDev, smDev, cw, ch, 0);
        CUDA_CHECK(cudaDeviceSynchronize());
        CUDA_CHECK(cudaMemcpy(dataMask.data(), dmDev, dataMask.size(),
                              cudaMemcpyDeviceToHost));
        CUDA_CHECK(cudaMemcpy(maxGrid.data(), mxDev,
                              maxGrid.size() * sizeof(unsigned int),
                              cudaMemcpyDeviceToHost));
        CUDA_CHECK(cudaMemcpy(sumGrid.data(), smDev,
                              sumGrid.size() * sizeof(unsigned int),
                              cudaMemcpyDeviceToHost));
        CUDA_CHECK(cudaFree(dmDev));
        CUDA_CHECK(cudaFree(mxDev));
        CUDA_CHECK(cudaFree(smDev));
        t.report();
    }

    if (const char* dp = std::getenv("DUMP_CELLS")) {
        FILE* f = std::fopen(dp, "wb");
        if (f) {
            const int hdr[4] = {cw, ch, D, T};
            std::fwrite(hdr, sizeof(int), 4, f);
            std::fwrite(maxGrid.data(), sizeof(unsigned int), maxGrid.size(), f);
            std::fwrite(sumGrid.data(), sizeof(unsigned int), sumGrid.size(), f);
            if (land->ready() && land->coarseW() == cw && land->coarseH() == ch) {
                std::fwrite(land->coarseRaw().data(), 1,
                            land->coarseRaw().size(), f);
            }
            std::fclose(f);
            std::fprintf(stderr, "[cells] dumped %dx%d decim %d tile %d -> %s\n",
                         cw, ch, D, T, dp);
        }
    }
    const uint8_t* dmHost = dataMask.data();
    const unsigned int* mxHost = maxGrid.data();

    std::vector<uint8_t> cfarCand;
    if (cfg.cfar_thresh > 0 && land->ready()
        && land->coarseW() == cw && land->coarseH() == ch) {
        Timer t("cfar screen");
        const int R = 16, G = 3;
        const size_t iw = size_t(cw) + 1;
        std::vector<double> ii(iw * (size_t(ch) + 1), 0.0);
        for (int y = 0; y < ch; ++y) {
            double rs = 0.0;
            for (int x = 0; x < cw; ++x) {
                rs += double(sumGrid[size_t(y) * cw + x]) / double(D * D);
                ii[(size_t(y) + 1) * iw + size_t(x) + 1] = ii[size_t(y) * iw + size_t(x) + 1] + rs;
            }
        }
        auto boxSum = [&](int x0, int y0, int x1, int y1, double& area) {
            x0 = std::max(0, x0); y0 = std::max(0, y0);
            x1 = std::min(cw, x1); y1 = std::min(ch, y1);
            area = double(x1 - x0) * double(y1 - y0);
            return ii[size_t(y1) * iw + size_t(x1)] - ii[size_t(y0) * iw + size_t(x1)]
                 - ii[size_t(y1) * iw + size_t(x0)] + ii[size_t(y0) * iw + size_t(x0)];
        };
        const std::vector<uint8_t>& lm = land->coarseRaw();
        cfarCand.assign(size_t(cw) * ch, 0);
        std::atomic<size_t> nc{0};
        const unsigned hc = std::thread::hardware_concurrency();
        const int nth = int(hc > 2 ? hc - 1 : 1);
        std::vector<std::thread> pool;
        for (int th = 0; th < nth; ++th) {
            pool.emplace_back([&, th]() {
                size_t local = 0;
                for (int y = th; y < ch; y += nth) {
                    for (int x = 0; x < cw; ++x) {
                        const size_t k = size_t(y) * cw + size_t(x);
                        if (!maxGrid[k] || lm[k]) continue;
                        double ab = 0.0, ag = 0.0;
                        const double sb = boxSum(x - R, y - R, x + R + 1, y + R + 1, ab);
                        const double sg = boxSum(x - G, y - G, x + G + 1, y + G + 1, ag);
                        const double area = ab - ag;
                        if (area <= 0.0) continue;
                        const double bg = (sb - sg) / area;
                        if (double(maxGrid[k]) - bg > double(cfg.cfar_thresh)) {
                            cfarCand[k] = 1;
                            ++local;
                        }
                    }
                }
                nc += local;
            });
        }
        for (auto& x : pool) x.join();
        std::fprintf(stderr, "[cfar] %zu water cells above local background + %d\n",
                     nc.load(), cfg.cfar_thresh);
        t.report();
    }

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

    {
        Timer t("tile triage");

        auto tileHasData = [&](int x0, int y0) {
            const int cx0 = x0 / D, cy0 = y0 / D;
            const int cx1 = std::min(cw - 1, (x0 + T - 1) / D);
            const int cy1 = std::min(ch - 1, (y0 + T - 1) / D);
            for (int y = cy0; y <= cy1; ++y) {
                const uint8_t* row = dmHost + size_t(y) * cw;
                for (int x = cx0; x <= cx1; ++x) if (row[x]) return true;
            }
            return false;
        };

        auto tileHasCfar = [&](int x0, int y0) {
            const int cx0 = x0 / D, cy0 = y0 / D;
            const int cx1 = std::min(cw - 1, (x0 + T - 1) / D);
            const int cy1 = std::min(ch - 1, (y0 + T - 1) / D);
            for (int y = cy0; y <= cy1; ++y) {
                const uint8_t* row = cfarCand.data() + size_t(y) * cw;
                for (int x = cx0; x <= cx1; ++x) if (row[x]) return true;
            }
            return false;
        };

        auto tileMax = [&](int x0, int y0) {
            const int cx0 = x0 / D, cy0 = y0 / D;
            const int cx1 = std::min(cw - 1, (x0 + T - 1) / D);
            const int cy1 = std::min(ch - 1, (y0 + T - 1) / D);
            unsigned int m = 0;
            for (int y = cy0; y <= cy1; ++y) {
                const unsigned int* row = mxHost + size_t(y) * cw;
                for (int x = cx0; x <= cx1; ++x) m = std::max(m, row[x]);
            }
            return m;
        };

        std::vector<int2> kept;
        kept.reserve(tiles.size());
        size_t droppedNodata = 0, droppedLand = 0, droppedBlack = 0, droppedCfar = 0;
        size_t histo[9] = {0, 0, 0, 0, 0, 0, 0, 0, 0};

        for (const int2& o : tiles) {
            if (!tileHasData(o.x, o.y)) { ++droppedNodata; continue; }
            if (land->ready() && land->tileFullyLand(o.x, o.y, T)) {
                ++droppedLand;
                continue;
            }
            const unsigned int m = tileMax(o.x, o.y);
            histo[m == 0 ? 0 : (m < 8 ? 1 : (m < 16 ? 2 : (m < 32 ? 3 :
                   (m < 64 ? 4 : (m < 96 ? 5 : (m < 128 ? 6 : (m < 192 ? 7 : 8)))))))]++;
            if (m <= cfg.tile_min_bright) { ++droppedBlack; continue; }
            if (!cfarCand.empty() && !tileHasCfar(o.x, o.y)) { ++droppedCfar; continue; }
            kept.push_back(o);
        }
        tiles.swap(kept);
        t.report();
        std::fprintf(stderr, "[tiles] %zu total -> %zu inferred "
                             "(%zu out-of-swath, %zu fully land, %zu below bright %d, %zu no cfar)\n",
                     nTilesRaw, tiles.size(), droppedNodata, droppedLand,
                     droppedBlack, cfg.tile_min_bright, droppedCfar);
        std::fprintf(stderr, "[tiles] peak-brightness histogram  =0:%zu  1-7:%zu  8-15:%zu"
                             "  16-31:%zu  32-63:%zu  64-95:%zu  96-127:%zu"
                             "  128-191:%zu  192+:%zu\n",
                     histo[0], histo[1], histo[2], histo[3], histo[4],
                     histo[5], histo[6], histo[7], histo[8]);
    }

    if (tiles.empty()) {
        std::fprintf(stderr, "[tiles] nothing to infer\n");
    }

    const std::string stem = std::filesystem::path(scene.basename()).stem().string();
    std::future<std::vector<CropWin>> fCrops;
    if (!cfg.out_crops.empty()) {
        const uint8_t* landRaw =
            (land->coarseW() == cw && land->coarseH() == ch && !land->coarseRaw().empty())
                ? land->coarseRaw().data() : nullptr;
        if (!landRaw)
            std::fprintf(stderr, "[crops] no land raster for this scene; "
                                 "every crop will be treated as open sea\n");

        fCrops = std::async(std::launch::async, [&, landRaw]() {
            Timer t("crop plan+encode (overlapped)");
            CoarseStats stats;
            stats.build(W, H, D, cw, ch, landRaw, dmHost);
            std::vector<CropWin> wins = planCrops(W, H, stats, cfg.crop);
            encodeCrops(grayHost, W, H, wins, cfg.crop, cfg.out_crops, stem);
            t.report();
            return wins;
        });
    }

    int lanes = 0;
    std::vector<StreamCtx> lane;
    Det* dDets = nullptr;
    int* dCount = nullptr;
    {
        Timer t("stream+lane setup");
        lanes = std::max(1, cfg.streams);
        if (!eng->createContexts(lanes)) FATAL("could not create %d TRT contexts", lanes);

        lane.resize(lanes);
        for (int i = 0; i < lanes; ++i) {
            CUDA_CHECK(cudaStreamCreateWithFlags(&lane[i].stream, cudaStreamNonBlocking));
            CUDA_CHECK(cudaMalloc(&lane[i].dIn,  eng->inputElems()  * sizeof(float)));
            CUDA_CHECK(cudaMalloc(&lane[i].dOut, eng->outputElems() * sizeof(float)));
            CUDA_CHECK(cudaMalloc(&lane[i].dOrig, size_t(B) * sizeof(int2)));
        }

        CUDA_CHECK(cudaMalloc(&dDets, size_t(cfg.max_det) * sizeof(Det)));
        CUDA_CHECK(cudaMalloc(&dCount, sizeof(int)));
        CUDA_CHECK(cudaMemset(dCount, 0, sizeof(int)));
        t.report();
    }

    {
        Timer t("inference");
        const size_t nBatches = (tiles.size() + B - 1) / B;
        for (size_t bi = 0; bi < nBatches; ++bi) {
            StreamCtx& L = lane[bi % lanes];
            const size_t off = bi * B;
            const int n = int(std::min<size_t>(B, tiles.size() - off));

            std::vector<int2> batch(B, tiles[off + n - 1]);
            std::copy(tiles.begin() + off, tiles.begin() + off + n, batch.begin());

            CUDA_CHECK(cudaMemcpyAsync(L.dOrig, batch.data(), size_t(B) * sizeof(int2),
                                       cudaMemcpyHostToDevice, L.stream));

            launch_preprocess(grayDev, W, H, L.dOrig, B, T, L.dIn, L.stream);

            if (!eng->enqueue(int(bi % lanes), L.dIn, L.dOut, L.stream))
                FATAL("enqueueV3 failed on batch %zu", bi);

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

    std::vector<Det> dets(nRaw);
    if (nRaw > 0) {
        Timer t("rotated NMS");
        CUDA_CHECK(cudaMemcpy(dets.data(), dDets, size_t(nRaw) * sizeof(Det),
                              cudaMemcpyDeviceToHost));

        std::sort(dets.begin(), dets.end(), [](const Det& a, const Det& b) {
            if (a.score != b.score) return a.score > b.score;
            if (a.cx    != b.cx)    return a.cx < b.cx;
            if (a.cy    != b.cy)    return a.cy < b.cy;
            if (a.w     != b.w)     return a.w < b.w;
            if (a.h     != b.h)     return a.h < b.h;
            return a.angle < b.angle;
        });
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

        const double ux = (d.w >= d.h) ? ca : -sa;
        const double uy = (d.w >= d.h) ? sa :  ca;
        double bearing = 0.0;
        scene.pixelDirToBearing(d.cx, d.cy, ux, uy, bearing);
        g.heading_deg = std::fmod(std::fmod(bearing, 180.0) + 180.0, 180.0);

        g.score = d.score;
        out.push_back(g);
    }

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

        {
            const double cx[4] = {0.0, double(W), double(W), 0.0};
            const double cy[4] = {0.0, 0.0, double(H), double(H)};
            f << "  \"footprint_lonlat\": [";
            for (int k = 0; k < 4; ++k) {
                double lon = 0.0, lat = 0.0;
                scene.pixelToLonLat(cx[k], cy[k], lon, lat);
                f << "[" << lon << ", " << lat << "]" << (k < 3 ? ", " : "");
            }
            f << "],\n";
        }

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

    if (fCrops.valid()) {
        Timer t("crop join + manifest");
        std::vector<CropWin> wins = fCrops.get();
        writeCropManifest(cfg.out_crops + "/manifest.json",
                          scene, stem, wins, cfg.crop, sea);
        t.report();
    }

    Det* dSea = nullptr;
    if (!sea.empty() && (!cfg.out_overview.empty() || !cfg.out_jpg.empty())) {
        CUDA_CHECK(cudaMalloc(&dSea, sea.size() * sizeof(Det)));
        CUDA_CHECK(cudaMemcpy(dSea, sea.data(), sea.size() * sizeof(Det),
                              cudaMemcpyHostToDevice));
    }

    if (!cfg.out_overview.empty()) {
        Timer t("overview render");
        const int f = std::max(1, (std::max(W, H) + cfg.overview_max - 1) / cfg.overview_max);
        const int ow = (W + f - 1) / f, oh = (H + f - 1) / f;

        uint8_t* dRGB = nullptr;
        if (cudaMalloc(&dRGB, size_t(ow) * oh * 3) != cudaSuccess) {
            std::fprintf(stderr, "[jpeg] cannot allocate the overview buffer; skipping\n");
        } else {
            launch_overview_rgb(grayDev, W, H, f, dRGB, ow, oh, 0);
            if (dSea && cfg.box_thickness > 0)
                launch_draw_boxes(dRGB, ow, oh, dSea, int(sea.size()),
                                  std::max(1, cfg.box_thickness / f), 1.0f / float(f), 0);
            CUDA_CHECK(cudaDeviceSynchronize());

            std::vector<uint8_t> host(size_t(ow) * oh * 3);
            CUDA_CHECK(cudaMemcpy(host.data(), dRGB, host.size(), cudaMemcpyDeviceToHost));
            CUDA_CHECK(cudaFree(dRGB));

            size_t bytes = 0;
            if (writeJpegRGB(host.data(), ow, oh, size_t(ow) * 3,
                             cfg.jpeg_quality, true, cfg.out_overview, &bytes))
                std::fprintf(stderr, "[overview] wrote %s  %dx%d (%d:1, %.2f MB)\n",
                             cfg.out_overview.c_str(), ow, oh, f, bytes / 1e6);
        }
        t.report();
    }

    if (!cfg.out_jpg.empty()) {
        Timer t("debug render");
        uint8_t* dRGB = nullptr;
        const size_t rgbBytes = size_t(W) * H * 3;
        if (cudaMalloc(&dRGB, rgbBytes) != cudaSuccess) {
            std::fprintf(stderr, "[jpeg] cannot allocate %.2f GB for the render; skipping\n",
                         rgbBytes / 1e9);
        } else {
            launch_render_rgb(grayDev, W, H, dRGB, 0);
            if (dSea && cfg.box_thickness > 0)
                launch_draw_boxes(dRGB, W, H, dSea, int(sea.size()),
                                  cfg.box_thickness, 1.0f, 0);
            CUDA_CHECK(cudaDeviceSynchronize());
            encodeJpegRGB(dRGB, W, H, cfg.jpeg_quality, cfg.out_jpg, 0);
            CUDA_CHECK(cudaFree(dRGB));
        }
        t.report();
    }

    if (dSea) CUDA_CHECK(cudaFree(dSea));
    if (grayHost) CUDA_CHECK(cudaFreeHost(grayHost));
    else if (grayDev) CUDA_CHECK(cudaFree(grayDev));

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

namespace {

std::string g_respPath;
std::string g_statsPath;
std::atomic<bool> g_jobActive{false};
std::atomic<bool> g_replied{true};

int openRespWriter(int waitSeconds) {
    for (int i = 0; i < waitSeconds * 100; ++i) {
        const int fd = ::open(g_respPath.c_str(), O_WRONLY | O_NONBLOCK);
        if (fd >= 0) return fd;
        if (errno != ENXIO) return -1;
        ::usleep(10000);
    }
    return -1;
}

void replyOnce(int rc, int waitSeconds) {
    if (g_replied.exchange(true)) return;
    const int fd = openRespWriter(waitSeconds);
    if (fd < 0) return;
    char out[32];
    const int len = std::snprintf(out, sizeof out, "%d\n", rc);
    ssize_t ignored = ::write(fd, out, size_t(len));
    (void)ignored;
    ::close(fd);
}

void replyOnExit() {
    if (g_jobActive.load()) replyOnce(1, 1);
}

void writeStats(const char* phase, long jobs, int lastRc, double lastMs, long startedAt) {
    if (g_statsPath.empty()) return;
    const std::string tmp = g_statsPath + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "w");
    if (!f) return;
    std::fprintf(f, "pid=%d\n", int(::getpid()));
    std::fprintf(f, "started=%ld\n", startedAt);
    std::fprintf(f, "updated=%ld\n", long(::time(nullptr)));
    std::fprintf(f, "phase=%s\n", phase);
    std::fprintf(f, "jobs=%ld\n", jobs);
    std::fprintf(f, "last_rc=%d\n", lastRc);
    std::fprintf(f, "last_ms=%.0f\n", lastMs);
    std::fclose(f);
    ::chmod(tmp.c_str(), 0666);
    ::rename(tmp.c_str(), g_statsPath.c_str());
}

int serveLoop(const std::string& dir) {
    const std::string reqPath = dir + "/req";
    g_respPath = dir + "/resp";
    g_statsPath = dir + "/stats";
    ::mkfifo(reqPath.c_str(), 0666);
    ::mkfifo(g_respPath.c_str(), 0666);
    ::chmod(reqPath.c_str(), 0666);
    ::chmod(g_respPath.c_str(), 0666);

    std::atexit(replyOnExit);

    const long startedAt = long(::time(nullptr));
    long jobs = 0;
    int lastRc = 0;
    double lastMs = 0.0;

    CUDA_CHECK(cudaFree(nullptr));
    writeStats("ready", jobs, lastRc, lastMs, startedAt);
    std::fprintf(stderr, "[serve] ready, waiting on %s\n", reqPath.c_str());
    std::fflush(stderr);

    for (;;) {
        int rfd = ::open(reqPath.c_str(), O_RDONLY);
        if (rfd < 0) break;
        std::string line;
        char buf[4096];
        ssize_t n;
        while ((n = ::read(rfd, buf, sizeof buf)) > 0) line.append(buf, size_t(n));
        ::close(rfd);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
        if (line.empty()) continue;

        std::vector<std::string> tok;
        {
            std::stringstream ss(line);
            std::string t;
            while (std::getline(ss, t, '\t')) if (!t.empty()) tok.push_back(t);
        }
        if (tok.size() < 2) continue;

        const std::string logPath = tok[0];
        std::vector<char*> argv;
        argv.push_back(const_cast<char*>("sar_ship_detect"));
        for (size_t i = 1; i < tok.size(); ++i) argv.push_back(const_cast<char*>(tok[i].c_str()));

        const int so = ::dup(1), se = ::dup(2);
        const int lf = ::open(logPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0666);
        if (lf >= 0) { ::dup2(lf, 1); ::dup2(lf, 2); }

        g_replied.store(false);
        g_jobActive.store(true);
        writeStats("busy", jobs, lastRc, lastMs, startedAt);

        int rc = 1;
        bool recycle = false;
        Timer tJob("job");
        try {
            Config cfg = Config::parse(int(argv.size()), argv.data());
            rc = runOnce(cfg);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "[serve] job threw: %s\n", e.what());
            recycle = true;
        } catch (...) {
            std::fprintf(stderr, "[serve] job threw unknown exception\n");
            recycle = true;
        }
        lastMs = tJob.ms();
        std::fflush(stdout);
        std::fflush(stderr);
        if (lf >= 0) { ::dup2(so, 1); ::dup2(se, 2); ::close(lf); }
        ::close(so); ::close(se);

        replyOnce(rc, 10);
        g_jobActive.store(false);
        ++jobs;
        lastRc = rc;

        if (recycle) {
            writeStats("recycling", jobs, lastRc, lastMs, startedAt);
            std::fprintf(stderr, "[serve] job aborted mid-flight; exiting so the "
                                 "supervisor hands back a process with clean GPU state\n");
            std::fflush(stderr);
            return 0;
        }
        writeStats("ready", jobs, lastRc, lastMs, startedAt);
    }
    return 0;
}

}

int main(int argc, char** argv) {
    GDALAllRegister();
    CPLSetConfigOption("GDAL_NUM_THREADS", "ALL_CPUS");
    GDALSetCacheMax64(int64_t(1) << 30);
    CPLSetConfigOption("GDAL_CACHEMAX", "2048");

    if (argc >= 3 && std::string(argv[1]) == "--serve") return serveLoop(argv[2]);

    try {
        return runOnce(Config::parse(argc, argv));
    } catch (const std::exception& e) {
        std::fprintf(stderr, "[fatal] %s\n", e.what());
        return 1;
    }
}
