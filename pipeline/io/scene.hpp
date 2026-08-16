#pragma once
#include <memory>
#include <string>
#include <cstdint>

class GDALDataset;
class OGRSpatialReference;
class OGRCoordinateTransformation;

class Scene {
public:
    ~Scene();

    bool open(const std::string& path);

    bool readAll();

    bool setGeometry(int W, int H, const double gt[6], const char* wkt,
                     const std::string& base,
                     const std::string& startISO, const std::string& stopISO);

    bool allocateRaster(bool wantFloat);

    int      width()  const { return W_; }
    int      height() const { return H_; }
    float*   host()   const { return hostPtr_; }
    float*   device() const { return devPtr_; }
    size_t   bytes()  const { return size_t(W_) * H_ * sizeof(float); }

    void pixelToMap(double col, double row, double& x, double& y) const;
    bool pixelToLonLat(double col, double row, double& lon, double& lat) const;

    bool pixelDirToBearing(double col, double row,
                           double ux, double uy, double& bearing_deg) const;

    void bboxMap(double& minx, double& miny, double& maxx, double& maxy) const;
    OGRSpatialReference* srs() const { return srs_; }

    double gt(int i) const { return gt_[i]; }
    double pixelSizeX() const { return gt_[1]; }
    double pixelSizeY() const { return gt_[5]; }

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
