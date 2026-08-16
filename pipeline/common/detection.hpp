#pragma once
#include <string>
#include <vector>

struct Det {
    float cx, cy;
    float w, h;
    float angle;
    float score;
};

struct GeoDet {
    double px[4], py[4];
    double lon[4], lat[4];
    double center_lon, center_lat;
    double length_m, width_m;
    double heading_deg;
    float  score;
};
