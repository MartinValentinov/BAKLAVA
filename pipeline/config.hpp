#pragma once
#include <string>
#include <vector>

// All tunables live here. Defaults match what was agreed:
//   fixed dB window [-25, 0], native 640 tiles, conf 0.5, 100 m land buffer.
struct Config {
    // --- input -------------------------------------------------------------
    std::string tif;                       // GeoTIFF scene (float32 sigma0 VH)
    std::string onnx;                      // shape-frozen ONNX
    std::string engine;                    // .engine cache path (built if absent)
    std::vector<std::string> gshhg;        // GSHHS_f_L1.shp ... L6.shp

    // --- output ------------------------------------------------------------
    std::string out_json = "detections.json";
    std::string out_jpg;                   // empty = no debug render

    // --- model / tiling ----------------------------------------------------
    int   imgsz    = 640;                  // must match the frozen ONNX
    int   batch    = 8;                    // must match the frozen ONNX
    int   overlap  = 64;                   // px of overlap between tiles
    int   streams  = 2;                    // concurrent CUDA streams / TRT contexts
    bool  fast_scene_read = true;          // raw libtiff read, see scene.cpp

    // --- radiometry --------------------------------------------------------
    // g = clamp((10*log10(sigma0) - db_lo) / (db_hi - db_lo), 0, 1)
    float db_lo    = -25.0f;
    float db_hi    =   0.0f;

    // --- detection ---------------------------------------------------------
    float conf     = 0.50f;
    float nms_iou  = 0.30f;                // rotated IoU; ships moor in tight rows
    int   max_det  = 8192;

    // --- land mask ---------------------------------------------------------
    double buffer_m = 100.0;               // seaward buffer on the GSHHG shoreline
    int    coarse_decim = 8;               // land raster decimation for tile skipping
    bool   skip_land_tiles = true;         // exact-equivalent accelerator, see README
    bool   skip_nodata_tiles = true;

    // --- render ------------------------------------------------------------
    int  jpeg_quality = 90;
    int  box_thickness = 5;                // px, at full scene resolution

    // --- engine build ------------------------------------------------------
    bool fp16 = true;
    size_t workspace_mb = 4096;

    bool verbose = false;

    static Config parse(int argc, char** argv);
    static void usage(const char* prog);
};
