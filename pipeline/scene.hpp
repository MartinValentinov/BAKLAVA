#pragma once
#include <memory>
#include <string>
#include <cstdint>

class GDALDataset;
class OGRSpatialReference;
class OGRCoordinateTransformation;

// Owns the scene raster in pinned, device-mapped host memory.
//
// The Orin AGX is an integrated GPU (cudaDevAttrIntegrated == 1) with
// PageableMemoryAccess == 0, so the right move is cudaHostAlloc(...Mapped) and
// cudaHostGetDevicePointer: GDAL reads straight into memory the kernels can
// address. There is no 2.5 GB host-to-device copy anywhere in this pipeline.
class Scene {
public:
    ~Scene();

    bool open(const std::string& path);

    // Blocking full-band read into pinned memory. When `fastPath` is set, tries
    // a raw libtiff strip/tile read straight into the pinned buffer first --
    // GDAL's own RasterIO measured ~2.5x slower into this GPU-mapped memory
    // than a bulk libtiff read (see pipeline/README.md's perf notes). Falls
    // back to RasterIO for anything that isn't a single uncompressed
    // strip/tile covering the whole raster -- correctness never depends on
    // the fast path succeeding.
    bool readAll(bool fastPath);

    int      width()  const { return W_; }
    int      height() const { return H_; }
    float*   host()   const { return hostPtr_; }
    float*   device() const { return devPtr_; }
    size_t   bytes()  const { return size_t(W_) * H_ * sizeof(float); }

    // map coords (scene CRS) of a pixel centre
    void pixelToMap(double col, double row, double& x, double& y) const;
    // WGS84 lon/lat of a pixel centre
    bool pixelToLonLat(double col, double row, double& lon, double& lat) const;

    // Initial true-north bearing (degrees, 0-360) of a unit direction expressed
    // in pixel space (+x right, +y down), evaluated at a pixel location.
    // Computed by projecting two points to WGS84 and taking the geodetic
    // azimuth, so grid convergence is handled implicitly rather than by formula.
    bool pixelDirToBearing(double col, double row,
                           double ux, double uy, double& bearing_deg) const;

    void bboxMap(double& minx, double& miny, double& maxx, double& maxy) const;
    OGRSpatialReference* srs() const { return srs_; }

    double gt(int i) const { return gt_[i]; }
    double pixelSizeX() const { return gt_[1]; }
    double pixelSizeY() const { return gt_[5]; }

    // Scene-level acquisition time from the Sentinel-1 filename.
    // Returns ISO-8601 UTC. start/stop are both parsed; `acquisition` is start.
    const std::string& acquisitionISO() const { return acq_; }
    const std::string& startISO() const { return start_; }
    const std::string& stopISO()  const { return stop_; }
    const std::string& basename() const { return base_; }

private:
    GDALDataset* ds_ = nullptr;
    OGRSpatialReference* srs_ = nullptr;
    OGRCoordinateTransformation* toWgs_ = nullptr;

    int W_ = 0, H_ = 0;
    double gt_[6] = {0, 1, 0, 0, 0, 1};

    float* hostPtr_ = nullptr;
    float* devPtr_  = nullptr;

    std::string path_, base_, acq_, start_, stop_;

    void parseFilename();
};
