#include "config.hpp"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

void Config::usage(const char* prog) {
    std::fprintf(stderr,
"usage: %s --tif SCENE.tif --onnx MODEL.onnx [options]\n"
"\n"
"required:\n"
"  --tif PATH             float32 sigma0 VH GeoTIFF\n"
"  --onnx PATH            shape-frozen YOLOv8-OBB ONNX\n"
"\n"
"model / tiling:\n"
"  --engine PATH          .engine cache (default: <onnx>.engine)\n"
"  --imgsz N              default 640 (must match the frozen ONNX)\n"
"  --batch N              default 8   (must match the frozen ONNX)\n"
"  --overlap N            tile overlap in px, default 64\n"
"  --streams N            concurrent streams/contexts, default 2\n"
"\n"
"radiometry (matches the training render):\n"
"  --db-lo F              default -25\n"
"  --db-hi F              default 0\n"
"\n"
"detection:\n"
"  --conf F               default 0.50\n"
"  --nms-iou F            rotated IoU, default 0.30\n"
"  --max-det N            default 8192\n"
"\n"
"land mask:\n"
"  --gshhg PATH           GSHHS_f_L*.shp, repeatable\n"
"  --buffer-m F           seaward buffer, default 100\n"
"  --no-skip-land-tiles   run inference on land tiles too (slower, same output)\n"
"\n"
"output:\n"
"  --out-json PATH        default detections.json\n"
"  --out-jpg PATH         full-resolution debug render (omit to skip)\n"
"  --jpeg-quality N       default 90\n"
"  --box-thickness N      default 5\n"
"\n"
"misc:\n"
"  --no-fp16              build the engine without FP16\n"
"  --workspace-mb N       default 4096\n"
"  -v, --verbose\n", prog);
}

Config Config::parse(int argc, char** argv) {
    Config c;
    auto need = [&](int i) {
        if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", argv[i]); std::exit(2); }
        return argv[i + 1];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--tif")            { c.tif = need(i); ++i; }
        else if (a == "--onnx")           { c.onnx = need(i); ++i; }
        else if (a == "--engine")         { c.engine = need(i); ++i; }
        else if (a == "--gshhg")          { c.gshhg.emplace_back(need(i)); ++i; }
        else if (a == "--out-json")       { c.out_json = need(i); ++i; }
        else if (a == "--out-jpg")        { c.out_jpg = need(i); ++i; }
        else if (a == "--imgsz")          { c.imgsz = std::atoi(need(i)); ++i; }
        else if (a == "--batch")          { c.batch = std::atoi(need(i)); ++i; }
        else if (a == "--overlap")        { c.overlap = std::atoi(need(i)); ++i; }
        else if (a == "--streams")        { c.streams = std::atoi(need(i)); ++i; }
        else if (a == "--db-lo")          { c.db_lo = std::atof(need(i)); ++i; }
        else if (a == "--db-hi")          { c.db_hi = std::atof(need(i)); ++i; }
        else if (a == "--conf")           { c.conf = std::atof(need(i)); ++i; }
        else if (a == "--nms-iou")        { c.nms_iou = std::atof(need(i)); ++i; }
        else if (a == "--max-det")        { c.max_det = std::atoi(need(i)); ++i; }
        else if (a == "--buffer-m")       { c.buffer_m = std::atof(need(i)); ++i; }
        else if (a == "--coarse-decim")   { c.coarse_decim = std::atoi(need(i)); ++i; }
        else if (a == "--no-skip-land-tiles") { c.skip_land_tiles = false; }
        else if (a == "--jpeg-quality")   { c.jpeg_quality = std::atoi(need(i)); ++i; }
        else if (a == "--box-thickness")  { c.box_thickness = std::atoi(need(i)); ++i; }
        else if (a == "--no-fp16")        { c.fp16 = false; }
        else if (a == "--workspace-mb")   { c.workspace_mb = std::atoll(need(i)); ++i; }
        else if (a == "-v" || a == "--verbose") { c.verbose = true; }
        else if (a == "-h" || a == "--help") { usage(argv[0]); std::exit(0); }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); usage(argv[0]); std::exit(2); }
    }

    if (c.tif.empty() || c.onnx.empty()) { usage(argv[0]); std::exit(2); }
    if (c.engine.empty()) c.engine = c.onnx + ".engine";
    return c;
}
