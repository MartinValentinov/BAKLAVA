#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

class Scene;

struct L1Config {
    std::string safe;
    std::string pol = "VH";
    std::string dem;
    std::string geoid;
    std::string out_l2;
    double pixel_spacing = 10.0;
    int    epsg = 0;
    int    coarse_step = 64;
    bool   no_geoid_ok = false;
};

struct L1Quant {
    bool  enabled = false;

    bool  hostVisible = true;
    int   decim   = 8;
    float db_lo   = -25.0f;
    float db_hi   =   0.0f;

    uint8_t* grayHost = nullptr;
    uint8_t* grayDev  = nullptr;
    int      cw = 0, ch = 0;
    std::vector<uint8_t>      dataMask;
    std::vector<unsigned int> maxGrid;
    std::vector<unsigned int> sumGrid;
};

bool runL1ToL2(const L1Config& cfg, Scene& scene,
               const std::function<void()>& onGeometryReady = {},
               L1Quant* quant = nullptr);
