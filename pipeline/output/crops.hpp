#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct Det;
class Scene;

struct CropConfig {
    int   size     = 1024;
    int   quality  = 80;
    int   thumb    = 256;
    int   threads  = 0;
    float min_sea  = 0.0f;
    float coast_lo = 0.02f;
    bool  coast    = true;
};

struct CropWin {
    int   x = 0, y = 0, size = 0;
    float sea_frac = 0.0f;
    float land_frac = 0.0f;
    bool  is_coast = false;

    bool     written = false;
    size_t   bytes = 0;
    size_t   thumb_bytes = 0;
    uint32_t crc = 0;
    uint32_t thumb_crc = 0;
};

class CoarseStats {
public:
    void build(int W, int H, int decim, int cw, int ch,
               const uint8_t* landRaw, const uint8_t* dataMask);

    int landCells(int x, int y, int w, int h) const;
    int seaCells (int x, int y, int w, int h) const;
    int cellsIn  (int x, int y, int w, int h) const;

    int decim() const { return decim_; }

private:
    int W_ = 0, H_ = 0, decim_ = 1, cw_ = 0, ch_ = 0;
    std::vector<int32_t> satLand_, satSea_;

    int rect(const std::vector<int32_t>& sat, int x, int y, int w, int h) const;
};

std::vector<CropWin> planCrops(int W, int H, const CoarseStats& stats,
                               const CropConfig& cfg);

size_t encodeCrops(const uint8_t* gray, int W, int H,
                   std::vector<CropWin>& wins, const CropConfig& cfg,
                   const std::string& dir, const std::string& stem);

bool writeCropManifest(const std::string& path, const Scene& scene,
                       const std::string& stem,
                       const std::vector<CropWin>& wins, const CropConfig& cfg,
                       const std::vector<Det>& dets);
