#pragma once
#include <string>
#include <vector>

// One oriented detection in SCENE pixel coordinates.
// angle is the Ultralytics OBB rotation in radians, range [-pi/4, 3pi/4).
struct Det {
    float cx, cy;      // centre, scene pixels
    float w, h;        // box dimensions, scene pixels
    float angle;       // radians
    float score;
};

// Fully resolved detection, ready for JSON.
struct GeoDet {
    // 4 corners, image order, closed clockwise in pixel space
    double px[4], py[4];        // scene pixel coords
    double lon[4], lat[4];      // WGS84
    double center_lon, center_lat;
    double length_m, width_m;   // long / short axis on the ground
    double heading_deg;         // 0-180, true north, axis-ambiguous by design
    float  score;
};
