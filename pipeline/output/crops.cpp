#include "crops.hpp"
#include "../common/detection.hpp"
#include "jpeg.hpp"
#include "../io/scene.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>
#include <thread>

namespace {

std::string cropId(const CropWin& c) {
    char buf[64];
    std::snprintf(buf, sizeof buf, "x%d_y%d", c.x, c.y);
    return buf;
}

uint32_t crc32File(const std::string& path) {
    static const std::array<uint32_t, 256> table = [] {
        std::array<uint32_t, 256> t{};
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
            t[i] = c;
        }
        return t;
    }();

    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (!f) return 0;
    uint32_t crc = 0xFFFFFFFFu;
    unsigned char buf[64 * 1024];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0)
        for (size_t i = 0; i < n; ++i)
            crc = table[(crc ^ buf[i]) & 0xFF] ^ (crc >> 8);
    std::fclose(f);
    return crc ^ 0xFFFFFFFFu;
}

}

void CoarseStats::build(int W, int H, int decim, int cw, int ch,
                        const uint8_t* landRaw, const uint8_t* dataMask) {
    W_ = W; H_ = H; decim_ = std::max(1, decim); cw_ = cw; ch_ = ch;

    const size_t rowN = size_t(cw_) + 1;
    satLand_.assign(rowN * (size_t(ch_) + 1), 0);
    satSea_ .assign(rowN * (size_t(ch_) + 1), 0);

    for (int y = 0; y < ch_; ++y) {
        int32_t rl = 0, rs = 0;
        const uint8_t* lrow = landRaw  ? landRaw  + size_t(y) * cw_ : nullptr;
        const uint8_t* drow = dataMask ? dataMask + size_t(y) * cw_ : nullptr;
        int32_t* ol = &satLand_[(size_t(y) + 1) * rowN];
        int32_t* os = &satSea_ [(size_t(y) + 1) * rowN];
        const int32_t* pl = &satLand_[size_t(y) * rowN];
        const int32_t* ps = &satSea_ [size_t(y) * rowN];
        for (int x = 0; x < cw_; ++x) {
            const bool land = lrow && lrow[x];
            const bool sea  = !land && drow && drow[x];
            rl += land ? 1 : 0;
            rs += sea  ? 1 : 0;
            ol[x + 1] = pl[x + 1] + rl;
            os[x + 1] = ps[x + 1] + rs;
        }
    }
}

int CoarseStats::rect(const std::vector<int32_t>& sat,
                      int x, int y, int w, int h) const {
    if (sat.empty()) return 0;
    int cx0 = (x + decim_ - 1) / decim_;
    int cy0 = (y + decim_ - 1) / decim_;
    int cx1 = (x + w) / decim_;
    int cy1 = (y + h) / decim_;
    cx0 = std::max(0, std::min(cx0, cw_));
    cy0 = std::max(0, std::min(cy0, ch_));
    cx1 = std::max(cx0, std::min(cx1, cw_));
    cy1 = std::max(cy0, std::min(cy1, ch_));

    const size_t rowN = size_t(cw_) + 1;
    return sat[size_t(cy1) * rowN + cx1] - sat[size_t(cy0) * rowN + cx1]
         - sat[size_t(cy1) * rowN + cx0] + sat[size_t(cy0) * rowN + cx0];
}

int CoarseStats::landCells(int x, int y, int w, int h) const { return rect(satLand_, x, y, w, h); }
int CoarseStats::seaCells (int x, int y, int w, int h) const { return rect(satSea_,  x, y, w, h); }

int CoarseStats::cellsIn(int x, int y, int w, int h) const {
    const int cx0 = std::max(0, std::min((x + decim_ - 1) / decim_, cw_));
    const int cy0 = std::max(0, std::min((y + decim_ - 1) / decim_, ch_));
    const int cx1 = std::max(cx0, std::min((x + w) / decim_, cw_));
    const int cy1 = std::max(cy0, std::min((y + h) / decim_, ch_));
    return (cx1 - cx0) * (cy1 - cy0);
}

namespace {

struct Frac { float sea, land; int total; };

Frac fractions(const CoarseStats& st, int x, int y, int c) {
    Frac f{};
    f.total = st.cellsIn(x, y, c, c);
    if (f.total <= 0) return f;
    f.sea  = float(st.seaCells (x, y, c, c)) / float(f.total);
    f.land = float(st.landCells(x, y, c, c)) / float(f.total);
    return f;
}

int overlap1D(int a0, int a1, int b0, int b1) {
    return std::max(0, std::min(a1, b1) - std::max(a0, b0));
}

}

std::vector<CropWin> planCrops(int W, int H, const CoarseStats& stats,
                               const CropConfig& cfg) {
    std::vector<CropWin> out;
    const int C = cfg.size;
    if (C <= 0 || W < C || H < C) {
        std::fprintf(stderr, "[crops] scene %dx%d smaller than one %d px crop; "
                             "no crops emitted\n", W, H, C);
        return out;
    }

    std::vector<int> gx, gy;
    for (int x = 0; x < W; x += C) gx.push_back(std::min(x, W - C));
    for (int y = 0; y < H; y += C) gy.push_back(std::min(y, H - C));
    gx.erase(std::unique(gx.begin(), gx.end()), gx.end());
    gy.erase(std::unique(gy.begin(), gy.end()), gy.end());

    std::vector<CropWin> coast;
    std::set<std::pair<int, int>> seenCoast;
    const int step = std::max(stats.decim(), C / 32);
    const int quant = std::max(1, C / 4);

    if (cfg.coast) {
        for (int cy : gy) {
            for (int cx : gx) {
                const Frac f = fractions(stats, cx, cy, C);
                if (f.total <= 0 || f.sea <= 0.0f) continue;
                if (f.land <= cfg.coast_lo) continue;

                float bestCost = 1e30f;
                int bx = cx, by = cy;
                bool found = false;
                for (int dy = -C / 2; dy <= C / 2; dy += step) {
                    for (int dx = -C / 2; dx <= C / 2; dx += step) {
                        const int x = std::max(0, std::min(cx + dx, W - C));
                        const int y = std::max(0, std::min(cy + dy, H - C));
                        const Frac g = fractions(stats, x, y, C);
                        if (g.total <= 0) continue;

                        if (g.sea + g.land < 0.75f) continue;
                        if (g.sea <= 0.0f || g.land <= 0.0f) continue;
                        const float cost = std::fabs(g.land - 0.5f)
                                         + 0.05f * float(std::abs(dx) + std::abs(dy)) / float(C);
                        if (cost < bestCost) { bestCost = cost; bx = x; by = y; found = true; }
                    }
                }
                if (!found) continue;
                if (!seenCoast.insert({bx / quant, by / quant}).second) continue;

                const Frac g = fractions(stats, bx, by, C);
                CropWin w;
                w.x = bx; w.y = by; w.size = C;
                w.sea_frac = g.sea; w.land_frac = g.land;
                w.is_coast = true;
                coast.push_back(w);
            }
        }
    }

    for (int cy : gy) {
        for (int cx : gx) {
            const Frac f = fractions(stats, cx, cy, C);
            if (f.total <= 0) continue;

            const int seaHere = stats.seaCells(cx, cy, C, C);
            if (seaHere == 0) continue;
            if (f.sea < cfg.min_sea) continue;

            int bestCovered = 0;
            for (const CropWin& w : coast) {
                const int ox = overlap1D(cx, cx + C, w.x, w.x + C);
                const int oy = overlap1D(cy, cy + C, w.y, w.y + C);
                if (ox <= 0 || oy <= 0) continue;
                const int ix = std::max(cx, w.x), iy = std::max(cy, w.y);
                bestCovered = std::max(bestCovered, stats.seaCells(ix, iy, ox, oy));
            }
            if (float(seaHere - bestCovered) / float(f.total) <= cfg.min_sea)
                continue;

            CropWin w;
            w.x = cx; w.y = cy; w.size = C;
            w.sea_frac = f.sea; w.land_frac = f.land;
            w.is_coast = false;
            out.push_back(w);
        }
    }

    out.insert(out.end(), coast.begin(), coast.end());
    std::sort(out.begin(), out.end(), [](const CropWin& a, const CropWin& b) {
        return a.y != b.y ? a.y < b.y : a.x < b.x;
    });

    size_t nCoast = 0;
    for (const CropWin& w : out) nCoast += w.is_coast ? 1 : 0;
    std::fprintf(stderr, "[crops] %zu windows of %d px (%zu open sea, %zu shoreline)\n",
                 out.size(), C, out.size() - nCoast, nCoast);
    return out;
}

size_t encodeCrops(const uint8_t* gray, int W, int H,
                   std::vector<CropWin>& wins, const CropConfig& cfg,
                   const std::string& dir, const std::string& stem) {
    if (wins.empty()) return 0;

    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const bool wantThumb = cfg.thumb > 0 && cfg.thumb < cfg.size;
    const std::string thumbDir = dir + "/thumbs";
    if (wantThumb) std::filesystem::create_directories(thumbDir, ec);

    int nThreads = cfg.threads;
    if (nThreads <= 0) {
        const unsigned hc = std::thread::hardware_concurrency();
        nThreads = int(hc > 3 ? hc - 2 : 1);
    }
    nThreads = std::max(1, std::min<int>(nThreads, int(wins.size())));

    std::atomic<size_t> next{0};
    std::atomic<size_t> total{0};

    auto worker = [&]() {
        const int C = cfg.size;
        std::vector<uint8_t> buf(size_t(C) * C);
        std::vector<uint8_t> tbuf;
        std::vector<uint32_t> acc;
        if (wantThumb) {
            tbuf.resize(size_t(cfg.thumb) * cfg.thumb);
            acc.resize(size_t(cfg.thumb) * cfg.thumb);
        }

        for (;;) {
            const size_t i = next.fetch_add(1);
            if (i >= wins.size()) break;
            CropWin& c = wins[i];

            if (c.size != C || c.x < 0 || c.y < 0 || c.x + C > W || c.y + C > H) {
                std::fprintf(stderr, "[crops] skipping out-of-range window %d,%d %d\n",
                             c.x, c.y, c.size);
                continue;
            }

            for (int r = 0; r < C; ++r)
                std::memcpy(buf.data() + size_t(r) * C,
                            gray + size_t(c.y + r) * W + c.x, size_t(C));

            const std::string id = cropId(c);
            const std::string file = dir + "/" + stem + "_" + id + ".jpg";
            size_t nb = 0;
            if (!writeJpegGray(buf.data(), C, C, size_t(C), cfg.quality, true, file, &nb))
                continue;
            c.bytes = nb;
            c.crc = crc32File(file);
            c.written = true;
            total.fetch_add(nb);

            if (wantThumb) {
                const int T = cfg.thumb;
                const int f = C / T;
                std::fill(acc.begin(), acc.end(), 0u);
                for (int r = 0; r < T * f; ++r) {
                    const uint8_t* src = buf.data() + size_t(r) * C;
                    uint32_t* dst = acc.data() + size_t(r / f) * T;
                    for (int x = 0; x < T * f; ++x) dst[x / f] += src[x];
                }
                const uint32_t norm = uint32_t(f) * uint32_t(f);
                for (size_t k = 0; k < acc.size(); ++k)
                    tbuf[k] = uint8_t((acc[k] + norm / 2) / norm);

                const std::string tfile = thumbDir + "/" + stem + "_" + id + ".jpg";
                size_t tb = 0;

                if (writeJpegGray(tbuf.data(), T, T, size_t(T),
                                  std::max(40, cfg.quality - 15), true, tfile, &tb)) {
                    c.thumb_bytes = tb;
                    c.thumb_crc = crc32File(tfile);
                    total.fetch_add(tb);
                }
            }
        }
    };

    std::vector<std::thread> pool;
    pool.reserve(size_t(nThreads));
    for (int i = 0; i < nThreads; ++i) pool.emplace_back(worker);
    for (auto& t : pool) t.join();

    size_t written = 0;
    for (const CropWin& c : wins) written += c.written ? 1 : 0;
    std::fprintf(stderr, "[crops] encoded %zu/%zu on %d threads, %.1f MB total\n",
                 written, wins.size(), nThreads, total.load() / 1e6);
    return total.load();
}

bool writeCropManifest(const std::string& path, const Scene& scene,
                       const std::string& stem,
                       const std::vector<CropWin>& wins, const CropConfig& cfg,
                       const std::vector<Det>& dets) {
    std::ofstream f(path);
    if (!f) { std::fprintf(stderr, "[crops] cannot write %s\n", path.c_str()); return false; }
    f.precision(9);
    f << std::fixed;

    size_t totalBytes = 0, totalThumb = 0, nWritten = 0;
    for (const CropWin& c : wins) {
        if (!c.written) continue;
        ++nWritten;
        totalBytes += c.bytes;
        totalThumb += c.thumb_bytes;
    }

    f << "{\n";
    f << "  \"scene\": \"" << scene.basename() << "\",\n";
    f << "  \"acquisition_time\": \"" << scene.acquisitionISO() << "\",\n";
    f << "  \"scene_width\": "  << scene.width()  << ",\n";
    f << "  \"scene_height\": " << scene.height() << ",\n";
    f << "  \"pixel_size_m\": " << std::fabs(scene.pixelSizeX()) << ",\n";

    {
        const double cx[4] = {0.0, double(scene.width()), double(scene.width()), 0.0};
        const double cy[4] = {0.0, 0.0, double(scene.height()), double(scene.height())};
        f << "  \"footprint_lonlat\": [";
        for (int k = 0; k < 4; ++k) {
            double lon = 0.0, lat = 0.0;
            scene.pixelToLonLat(cx[k], cy[k], lon, lat);
            f << "[" << lon << ", " << lat << "]" << (k < 3 ? ", " : "");
        }
        f << "],\n";
    }
    f << "  \"crop_size\": "    << cfg.size    << ",\n";
    f << "  \"thumb_size\": "   << cfg.thumb   << ",\n";
    f << "  \"quality\": "      << cfg.quality << ",\n";
    f << "  \"count\": "        << nWritten    << ",\n";
    f << "  \"bytes_full\": "   << totalBytes  << ",\n";
    f << "  \"bytes_thumb\": "  << totalThumb  << ",\n";
    f << "  \"crops\": [\n";

    bool first = true;
    for (const CropWin& c : wins) {
        if (!c.written) continue;
        if (!first) f << ",\n";
        first = false;

        const std::string id = cropId(c);
        double lon[4], lat[4], clon, clat;
        const double cxs[4] = {double(c.x), double(c.x + c.size),
                               double(c.x + c.size), double(c.x)};
        const double cys[4] = {double(c.y), double(c.y),
                               double(c.y + c.size), double(c.y + c.size)};
        for (int k = 0; k < 4; ++k) scene.pixelToLonLat(cxs[k], cys[k], lon[k], lat[k]);
        scene.pixelToLonLat(c.x + c.size * 0.5, c.y + c.size * 0.5, clon, clat);

        f << "    {\n";
        f << "      \"id\": \"" << id << "\",\n";
        f << "      \"file\": \"" << stem << "_" << id << ".jpg\",\n";
        if (c.thumb_bytes)
            f << "      \"thumb\": \"thumbs/" << stem << "_" << id << ".jpg\",\n";
        f << "      \"kind\": \"" << (c.is_coast ? "coast" : "sea") << "\",\n";
        f << "      \"x\": " << c.x << ", \"y\": " << c.y
          << ", \"w\": " << c.size << ", \"h\": " << c.size << ",\n";
        f << "      \"sea_frac\": "  << c.sea_frac  << ",\n";
        f << "      \"land_frac\": " << c.land_frac << ",\n";
        f << "      \"bytes\": " << c.bytes
          << ", \"thumb_bytes\": " << c.thumb_bytes << ",\n";
        f << "      \"crc32\": " << c.crc
          << ", \"thumb_crc32\": " << c.thumb_crc << ",\n";
        f << "      \"center\": {\"lon\": " << clon << ", \"lat\": " << clat << "},\n";
        f << "      \"corners_lonlat\": [";
        for (int k = 0; k < 4; ++k)
            f << "[" << lon[k] << ", " << lat[k] << "]" << (k < 3 ? ", " : "");
        f << "],\n";

        f << "      \"detections\": [";
        bool firstD = true;
        for (size_t d = 0; d < dets.size(); ++d) {
            if (dets[d].cx < c.x || dets[d].cx >= c.x + c.size) continue;
            if (dets[d].cy < c.y || dets[d].cy >= c.y + c.size) continue;
            if (!firstD) f << ", ";
            firstD = false;
            f << d;
        }
        f << "]\n";
        f << "    }";
    }
    f << "\n  ]\n}\n";
    std::fprintf(stderr, "[crops] wrote %s (%zu crops, %.1f MB full + %.1f MB thumbs)\n",
                 path.c_str(), nWritten, totalBytes / 1e6, totalThumb / 1e6);
    return true;
}
