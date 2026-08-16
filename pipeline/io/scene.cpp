#include "scene.hpp"
#include "../common/util.hpp"

#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>
#include <tiffio.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <regex>
#include <filesystem>
#include <vector>

static constexpr double DEG = 180.0 / M_PI;
static constexpr double RAD = M_PI / 180.0;

namespace {

constexpr int kProbeStride = 32;
constexpr int kEdgeSlack   = 64;

struct Span { int x0, x1; };

bool preadExact(int fd, void* dst, size_t want, uint64_t off) {
    size_t done = 0;
    while (done < want) {
        const ssize_t got = ::pread(fd, static_cast<char*>(dst) + done, want - done,
                                    off_t(off + done));
        if (got <= 0) return false;
        done += size_t(got);
    }
    return true;
}

Span positiveExtent(const float* row, int W) {
    int a = 0;
    while (a < W && !(row[a] > 0.0f)) ++a;
    if (a == W) return Span{0, 0};
    int b = W - 1;
    while (b > a && !(row[b] > 0.0f)) --b;
    return Span{a, b + 1};
}

enum class FootprintResult { Done, Declined, Failed };

FootprintResult footprintRead(int fd, uint64_t base, int W, int H, float* dst,
                              bool swab, size_t* bytesRead) {
    const size_t rowBytes = size_t(W) * sizeof(float);

    std::vector<int> py;
    for (int y = 0; y < H; y += kProbeStride) py.push_back(y);
    if (py.back() != H - 1) py.push_back(H - 1);
    if (py.size() < 2) return FootprintResult::Declined;

    std::vector<Span> ps(py.size());
    size_t bytes = 0;
    for (size_t i = 0; i < py.size(); ++i) {
        float* row = dst + size_t(py[i]) * W;
        if (!preadExact(fd, row, rowBytes, base + uint64_t(py[i]) * rowBytes))
            return FootprintResult::Failed;
        if (swab) TIFFSwabArrayOfFloat(row, tmsize_t(W));
        ps[i] = positiveExtent(row, W);
        bytes += rowBytes;
    }

    int step = 0;
    for (size_t i = 1; i < py.size(); ++i) {
        if (ps[i].x1 <= ps[i].x0 || ps[i - 1].x1 <= ps[i - 1].x0) continue;
        step = std::max(step, std::abs(ps[i].x0 - ps[i - 1].x0));
        step = std::max(step, std::abs(ps[i].x1 - ps[i - 1].x1));
    }
    const int margin = 2 * step + kEdgeSlack;

    std::vector<Span> band(py.size() - 1);
    size_t planned = 0;
    for (size_t i = 0; i + 1 < py.size(); ++i) {
        const Span& a = ps[i];
        const Span& b = ps[i + 1];
        const bool ea = a.x1 <= a.x0, eb = b.x1 <= b.x0;
        Span s{0, 0};
        if (!(ea && eb)) {
            int lo, hi;
            if (ea)      { lo = b.x0; hi = b.x1; }
            else if (eb) { lo = a.x0; hi = a.x1; }
            else         { lo = std::min(a.x0, b.x0); hi = std::max(a.x1, b.x1); }
            s.x0 = std::max(0, lo - margin);
            s.x1 = std::min(W, hi + margin);
            if (s.x1 - s.x0 > (W * 9) / 10) { s.x0 = 0; s.x1 = W; }
        }
        band[i] = s;
        planned += size_t(s.x1 - s.x0) * sizeof(float) * size_t(py[i + 1] - py[i] - 1);
    }

    if (bytes + planned > (size_t(W) * H * sizeof(float) * 9) / 10)
        return FootprintResult::Declined;

    for (size_t i = 0; i + 1 < py.size(); ++i) {
        const Span s = band[i];
        const int nx = s.x1 - s.x0;
        for (int y = py[i] + 1; y < py[i + 1]; ++y) {
            float* row = dst + size_t(y) * W;
            if (nx <= 0) { std::memset(row, 0, rowBytes); continue; }
            if (s.x0 > 0) std::memset(row, 0, size_t(s.x0) * sizeof(float));
            if (s.x1 < W) std::memset(row + s.x1, 0, size_t(W - s.x1) * sizeof(float));
            if (!preadExact(fd, row + s.x0, size_t(nx) * sizeof(float),
                            base + uint64_t(y) * rowBytes + uint64_t(s.x0) * sizeof(float)))
                return FootprintResult::Failed;
            if (swab) TIFFSwabArrayOfFloat(row + s.x0, tmsize_t(nx));
            if ((s.x0 > 0 && row[s.x0] > 0.0f) || (s.x1 < W && row[s.x1 - 1] > 0.0f))
                return FootprintResult::Declined;
            bytes += size_t(nx) * sizeof(float);
        }
    }

    *bytesRead = bytes;
    return FootprintResult::Done;
}

bool tryRawStripRead(const std::string& path, int W, int H, size_t nBytes, float* dst) {
    TIFF* tif = TIFFOpen(path.c_str(), "r");
    if (!tif) return false;

    bool layoutOk = false;
    uint64_t base = 0;
    bool swab = false;
    do {
        uint16_t bits = 0, fmt = 0, spp = 0, comp = 0;
        TIFFGetFieldDefaulted(tif, TIFFTAG_BITSPERSAMPLE, &bits);
        TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLEFORMAT, &fmt);
        TIFFGetFieldDefaulted(tif, TIFFTAG_SAMPLESPERPIXEL, &spp);
        TIFFGetFieldDefaulted(tif, TIFFTAG_COMPRESSION, &comp);
        if (bits != 32 || fmt != SAMPLEFORMAT_IEEEFP || spp != 1 || comp != COMPRESSION_NONE)
            break;

        uint64_t* offs = nullptr;
        uint64_t* counts = nullptr;
        if (TIFFIsTiled(tif)) {
            uint32_t tw = 0, tl = 0;
            TIFFGetField(tif, TIFFTAG_TILEWIDTH, &tw);
            TIFFGetField(tif, TIFFTAG_TILELENGTH, &tl);
            if (tw != uint32_t(W) || tl != uint32_t(H) || TIFFNumberOfTiles(tif) != 1) break;
            if (!TIFFGetField(tif, TIFFTAG_TILEOFFSETS, &offs)) break;
            if (!TIFFGetField(tif, TIFFTAG_TILEBYTECOUNTS, &counts)) break;
        } else {
            if (TIFFNumberOfStrips(tif) != 1) break;
            if (!TIFFGetField(tif, TIFFTAG_STRIPOFFSETS, &offs)) break;
            if (!TIFFGetField(tif, TIFFTAG_STRIPBYTECOUNTS, &counts)) break;
        }
        if (!offs || !counts || counts[0] != uint64_t(nBytes)) break;

        base = offs[0];
        swab = TIFFIsByteSwapped(tif) != 0;
        layoutOk = true;
    } while (false);

    TIFFClose(tif);
    if (!layoutOk) return false;

    const int fd = ::open(path.c_str(), O_RDONLY);
    if (fd < 0) return false;

    size_t bytes = 0;
    const FootprintResult fr = footprintRead(fd, base, W, H, dst, swab, &bytes);
    if (fr == FootprintResult::Done) {
        std::fprintf(stderr, "[scene] footprint read %.2f of %.2f GB (%.0f%% of the raster "
                             "is out-of-swath nodata)\n",
                     bytes / 1e9, nBytes / 1e9, 100.0 * (1.0 - double(bytes) / double(nBytes)));
        ::close(fd);
        return true;
    }

    const bool ok = preadExact(fd, dst, nBytes, base);
    ::close(fd);
    if (!ok) return false;
    if (swab) TIFFSwabArrayOfFloat(dst, tmsize_t(nBytes / sizeof(float)));
    if (fr == FootprintResult::Declined)
        std::fprintf(stderr, "[scene] footprint read declined; read all %.2f GB\n", nBytes / 1e9);
    return true;
}

}

Scene::~Scene() {
    if (toWgs_) OCTDestroyCoordinateTransformation(
        reinterpret_cast<OGRCoordinateTransformationH>(toWgs_));
    if (srs_) srs_->Release();
    if (hostPtr_) cudaFreeHost(hostPtr_);
    if (ds_) GDALClose(ds_);
}

bool Scene::open(const std::string& path) {
    GDALAllRegister();
    CPLSetConfigOption("GDAL_CACHEMAX", "2048");
    CPLSetConfigOption("GDAL_NUM_THREADS", "ALL_CPUS");

    ds_ = static_cast<GDALDataset*>(GDALOpen(path.c_str(), GA_ReadOnly));
    if (!ds_) { std::fprintf(stderr, "[scene] cannot open %s\n", path.c_str()); return false; }

    W_ = ds_->GetRasterXSize();
    H_ = ds_->GetRasterYSize();
    if (ds_->GetRasterCount() < 1) { std::fprintf(stderr, "[scene] no bands\n"); return false; }
    if (ds_->GetGeoTransform(gt_) != CE_None) {
        std::fprintf(stderr, "[scene] no geotransform\n");
        return false;
    }

    const char* wkt = ds_->GetProjectionRef();
    if (!wkt || !*wkt) { std::fprintf(stderr, "[scene] no projection\n"); return false; }
    srs_ = new OGRSpatialReference();
    srs_->importFromWkt(&wkt);
    srs_->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference wgs;
    wgs.importFromEPSG(4326);
    wgs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    toWgs_ = OGRCreateCoordinateTransformation(srs_, &wgs);
    if (!toWgs_) { std::fprintf(stderr, "[scene] cannot build transform to WGS84\n"); return false; }

    path_ = path;
    base_ = std::filesystem::path(path).filename().string();
    parseFilename();

    std::fprintf(stderr, "[scene] %s  %dx%d  px=%.2f m  acq=%s\n",
                 base_.c_str(), W_, H_, std::fabs(gt_[1]), acq_.c_str());
    return true;
}

void Scene::parseFilename() {
    std::regex re(R"((\d{8})T(\d{6})_(\d{8})T(\d{6}))");
    std::smatch m;
    auto iso = [](const std::string& d, const std::string& t) {
        return d.substr(0, 4) + "-" + d.substr(4, 2) + "-" + d.substr(6, 2) + "T" +
               t.substr(0, 2) + ":" + t.substr(2, 2) + ":" + t.substr(4, 2) + "Z";
    };
    if (std::regex_search(base_, m, re) && m.size() == 5) {
        start_ = iso(m[1].str(), m[2].str());
        stop_  = iso(m[3].str(), m[4].str());
    } else {
        const char* tag = ds_ ? ds_->GetMetadataItem("TIFFTAG_DATETIME") : nullptr;
        start_ = tag ? tag : "";
        stop_  = start_;
        std::fprintf(stderr, "[scene] WARNING filename has no S1 timestamps; "
                             "acquisition time may be empty\n");
    }
    acq_ = start_;
}

bool Scene::readAll() {
    const size_t n = bytes();
    CUDA_CHECK(cudaHostAlloc(reinterpret_cast<void**>(&hostPtr_), n,
                             cudaHostAllocMapped | cudaHostAllocPortable));
    CUDA_CHECK(cudaHostGetDevicePointer(reinterpret_cast<void**>(&devPtr_), hostPtr_, 0));

    if (ds_->GetRasterCount() == 1 && tryRawStripRead(path_, W_, H_, n, hostPtr_))
        return true;

    GDALRasterBand* b = ds_->GetRasterBand(1);
    CPLErr e = b->RasterIO(GF_Read, 0, 0, W_, H_, hostPtr_, W_, H_, GDT_Float32, 0, 0, nullptr);
    if (e != CE_None) { std::fprintf(stderr, "[scene] RasterIO failed\n"); return false; }
    return true;
}

bool Scene::setGeometry(int W, int H, const double gt[6], const char* wkt,
                        const std::string& base,
                        const std::string& startISO, const std::string& stopISO) {
    W_ = W; H_ = H;
    for (int i = 0; i < 6; ++i) gt_[i] = gt[i];

    srs_ = new OGRSpatialReference();
    const char* w = wkt;
    if (srs_->importFromWkt(&w) != OGRERR_NONE) {
        std::fprintf(stderr, "[scene] cannot parse the L2 projection\n");
        return false;
    }
    srs_->SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference wgs;
    wgs.importFromEPSG(4326);
    wgs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    toWgs_ = OGRCreateCoordinateTransformation(srs_, &wgs);
    if (!toWgs_) { std::fprintf(stderr, "[scene] cannot build transform to WGS84\n"); return false; }

    base_  = base + ".tif";
    start_ = startISO;
    stop_  = stopISO;
    acq_   = startISO;

    std::fprintf(stderr, "[scene] %s  %dx%d  px=%.2f m  acq=%s  (from L1)\n",
                 base_.c_str(), W_, H_, std::fabs(gt_[1]), acq_.c_str());
    return true;
}

bool Scene::allocateRaster(bool wantFloat) {
    if (!wantFloat) return true;
    CUDA_CHECK(cudaHostAlloc(reinterpret_cast<void**>(&hostPtr_), bytes(),
                             cudaHostAllocMapped | cudaHostAllocPortable));
    CUDA_CHECK(cudaHostGetDevicePointer(reinterpret_cast<void**>(&devPtr_), hostPtr_, 0));
    return true;
}

void Scene::pixelToMap(double col, double row, double& x, double& y) const {
    x = gt_[0] + col * gt_[1] + row * gt_[2];
    y = gt_[3] + col * gt_[4] + row * gt_[5];
}

bool Scene::pixelToLonLat(double col, double row, double& lon, double& lat) const {
    double x, y;
    pixelToMap(col, row, x, y);
    double zx = x, zy = y;
    if (!toWgs_->Transform(1, &zx, &zy)) return false;
    lon = zx; lat = zy;
    return true;
}

bool Scene::pixelDirToBearing(double col, double row,
                              double ux, double uy, double& bearing_deg) const {
    const double step_px = 50.0 / std::fabs(gt_[1]);
    double lon1, lat1, lon2, lat2;
    if (!pixelToLonLat(col, row, lon1, lat1)) return false;
    if (!pixelToLonLat(col + ux * step_px, row + uy * step_px, lon2, lat2)) return false;

    const double p1 = lat1 * RAD, p2 = lat2 * RAD;
    const double dl = (lon2 - lon1) * RAD;
    const double yy = std::sin(dl) * std::cos(p2);
    const double xx = std::cos(p1) * std::sin(p2) - std::sin(p1) * std::cos(p2) * std::cos(dl);
    double b = std::atan2(yy, xx) * DEG;
    if (b < 0) b += 360.0;
    bearing_deg = b;
    return true;
}

void Scene::bboxMap(double& minx, double& miny, double& maxx, double& maxy) const {
    double xs[4], ys[4];
    pixelToMap(0,  0,  xs[0], ys[0]);
    pixelToMap(W_, 0,  xs[1], ys[1]);
    pixelToMap(W_, H_, xs[2], ys[2]);
    pixelToMap(0,  H_, xs[3], ys[3]);
    minx = maxx = xs[0];
    miny = maxy = ys[0];
    for (int i = 1; i < 4; ++i) {
        minx = std::min(minx, xs[i]); maxx = std::max(maxx, xs[i]);
        miny = std::min(miny, ys[i]); maxy = std::max(maxy, ys[i]);
    }
}
