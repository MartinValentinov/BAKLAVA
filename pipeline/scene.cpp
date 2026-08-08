#include "scene.hpp"
#include "util.hpp"

#include <gdal_priv.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

#include <cmath>
#include <regex>
#include <filesystem>

static constexpr double DEG = 180.0 / M_PI;
static constexpr double RAD = M_PI / 180.0;

Scene::~Scene() {
    if (toWgs_) OCTDestroyCoordinateTransformation(
        reinterpret_cast<OGRCoordinateTransformationH>(toWgs_));
    if (srs_) srs_->Release();
    if (hostPtr_) cudaFreeHost(hostPtr_);
    if (ds_) GDALClose(ds_);
}

bool Scene::open(const std::string& path) {
    GDALAllRegister();
    // Large sequential reads; give GDAL a generous block cache.
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

    base_ = std::filesystem::path(path).filename().string();
    parseFilename();

    std::fprintf(stderr, "[scene] %s  %dx%d  px=%.2f m  acq=%s\n",
                 base_.c_str(), W_, H_, std::fabs(gt_[1]), acq_.c_str());
    return true;
}

// S1C_IW_GRDH_1SDV_20260807T041243_20260807T041308_008884_0119F6_3271.tif
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
        // Fall back to the TIFF tag, then to empty. Never invent a time.
        const char* tag = ds_ ? ds_->GetMetadataItem("TIFFTAG_DATETIME") : nullptr;
        start_ = tag ? tag : "";
        stop_  = start_;
        std::fprintf(stderr, "[scene] WARNING filename has no S1 timestamps; "
                             "acquisition time may be empty\n");
    }
    // Scene-level time for every detection in this scene: sensing start.
    acq_ = start_;
}

bool Scene::readAll() {
    const size_t n = bytes();
    // Mapped + pinned: device pointer aliases the same physical pages on Orin.
    CUDA_CHECK(cudaHostAlloc(reinterpret_cast<void**>(&hostPtr_), n,
                             cudaHostAllocMapped | cudaHostAllocPortable));
    CUDA_CHECK(cudaHostGetDevicePointer(reinterpret_cast<void**>(&devPtr_), hostPtr_, 0));

    GDALRasterBand* b = ds_->GetRasterBand(1);
    CPLErr e = b->RasterIO(GF_Read, 0, 0, W_, H_, hostPtr_, W_, H_, GDT_Float32, 0, 0, nullptr);
    if (e != CE_None) { std::fprintf(stderr, "[scene] RasterIO failed\n"); return false; }
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
    // Step ~50 m along the direction in pixel space, convert both ends to
    // geographic coordinates, take the initial great-circle azimuth.
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
