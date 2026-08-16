#pragma once
#include <string>
#include <vector>

#include "output/crops.hpp"
#include "l1/l1proc.hpp"

struct Config {
    std::string tif;
    L1Config l1;
    std::string onnx;
    std::string engine;
    std::vector<std::string> gshhg;

    std::string out_json = "detections.json";
    std::string out_jpg;
    std::string out_overview;
    std::string out_crops;
    int    overview_max = 4096;
    CropConfig crop;

    int   overlap  = 64;
    int   tile_min_bright = 0;
    int   cfar_thresh = 0;
    int   streams  = 2;

    float db_lo    = -25.0f;
    float db_hi    =   0.0f;

    float conf     = 0.50f;
    float nms_iou  = 0.30f;
    int   max_det  = 8192;

    double buffer_m = 100.0;
    int    coarse_decim = 8;

    int  jpeg_quality = 90;
    int  box_thickness = 5;

    size_t workspace_mb = 4096;

    static Config parse(int argc, char** argv);
    static void usage(const char* prog);
};
