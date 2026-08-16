#include "config.hpp"
#include <stdexcept>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

void Config::usage(const char* prog) {
    std::fprintf(stderr,
"usage: %s --tif SCENE.tif --onnx MODEL.onnx [options]\n"
"\n"
"required: --onnx, plus exactly one of --tif / --safe\n"
"  --tif PATH             float32 sigma0 GeoTIFF, already terrain corrected\n"
"  --safe PATH            raw Sentinel-1 L1 GRD .SAFE directory or .zip; it is\n"
"                         calibrated, denoised and range-Doppler geocoded on\n"
"                         the GPU straight into memory, no intermediate file\n"
"  --onnx PATH            shape-frozen YOLOv8-OBB ONNX\n"
"\n"
"L1 -> L2 (only with --safe):\n"
"  --pol VH|VV            polarisation, default VH\n"
"  --dem PATH             GDAL-readable DEM covering the scene (required for\n"
"                         terrain correction; omit for ellipsoid-only)\n"
"  --geoid PATH           geoid undulation grid added to the DEM. Required\n"
"                         with --dem unless --dem-is-ellipsoidal: SRTM heights\n"
"                         are orthometric and the ~35 m difference in the Black\n"
"                         Sea is a measured 4 px geolocation error\n"
"  --dem-is-ellipsoidal   the DEM already has ellipsoidal heights\n"
"  --pixel-spacing F      output metres per pixel, default 10\n"
"  --epsg N               output CRS, default 0 = the scene's UTM zone\n"
"  --coarse-step N        geocoding lattice step in output px, default 32\n"
"  --out-l2 PATH          also write the L2 GeoTIFF (costs a full extra write)\n"
"\n"
"model / tiling:\n"
"  --engine PATH          .engine cache (default: <onnx>.engine)\n"
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
"\n"
"output:\n"
"  --out-json PATH        default detections.json\n"
"  --out-jpg PATH         full-resolution debug render (omit to skip; slow)\n"
"  --out-overview PATH    decimated whole-scene render with boxes (fast)\n"
"  --overview-max N       longest side of the overview, default 4096\n"
"  --jpeg-quality N       default 90\n"
"  --box-thickness N      default 5\n"
"\n"
"water crops (omit --out-crops to skip all of this):\n"
"  --out-crops DIR        write one grayscale JPEG per water crop + manifest\n"
"  --crop-size N          square crop side in px, default 1024\n"
"  --crop-quality N       default 80\n"
"  --crop-thumb N         thumbnail side in px, default 256 (0 disables)\n"
"  --crop-min-sea F       allow this fraction of a crop's sea to go uncovered,\n"
"                         default 0 = cover every last pixel of water\n"
"  --crop-coast-lo F      land fraction above which a crop counts as coastal,\n"
"                         default 0.02\n"
"  --no-crop-coast        do not emit the extra ~50/50 shoreline crops\n"
"  --crop-threads N       encode threads, default 0 = cores - 2\n"
"\n"
"misc:\n"
"  --workspace-mb N       default 4096\n", prog);
}

Config Config::parse(int argc, char** argv) {
    Config c;
    auto need = [&](int i) {
        if (i + 1 >= argc) { throw std::runtime_error(std::string("missing value for ") + argv[i]); }
        return argv[i + 1];
    };

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        if      (a == "--tif")            { c.tif = need(i); ++i; }
        else if (a == "--safe")           { c.l1.safe = need(i); ++i; }
        else if (a == "--pol")            { c.l1.pol = need(i); ++i; }
        else if (a == "--dem")            { c.l1.dem = need(i); ++i; }
        else if (a == "--geoid")          { c.l1.geoid = need(i); ++i; }
        else if (a == "--dem-is-ellipsoidal") { c.l1.no_geoid_ok = true; }
        else if (a == "--pixel-spacing")  { c.l1.pixel_spacing = std::atof(need(i)); ++i; }
        else if (a == "--epsg")           { c.l1.epsg = std::atoi(need(i)); ++i; }
        else if (a == "--coarse-step")    { c.l1.coarse_step = std::atoi(need(i)); ++i; }
        else if (a == "--out-l2")         { c.l1.out_l2 = need(i); ++i; }
        else if (a == "--onnx")           { c.onnx = need(i); ++i; }
        else if (a == "--engine")         { c.engine = need(i); ++i; }
        else if (a == "--gshhg")          { c.gshhg.emplace_back(need(i)); ++i; }
        else if (a == "--out-json")       { c.out_json = need(i); ++i; }
        else if (a == "--out-jpg")        { c.out_jpg = need(i); ++i; }
        else if (a == "--out-overview")   { c.out_overview = need(i); ++i; }
        else if (a == "--overview-max")   { c.overview_max = std::atoi(need(i)); ++i; }
        else if (a == "--out-crops")      { c.out_crops = need(i); ++i; }
        else if (a == "--crop-size")      { c.crop.size = std::atoi(need(i)); ++i; }
        else if (a == "--crop-quality")   { c.crop.quality = std::atoi(need(i)); ++i; }
        else if (a == "--crop-thumb")     { c.crop.thumb = std::atoi(need(i)); ++i; }
        else if (a == "--crop-min-sea")   { c.crop.min_sea = float(std::atof(need(i))); ++i; }
        else if (a == "--crop-coast-lo")  { c.crop.coast_lo = float(std::atof(need(i))); ++i; }
        else if (a == "--crop-threads")   { c.crop.threads = std::atoi(need(i)); ++i; }
        else if (a == "--no-crop-coast")  { c.crop.coast = false; }
        else if (a == "--overlap")        { c.overlap = std::atoi(need(i)); ++i; }
        else if (a == "--tile-min-bright") { c.tile_min_bright = std::atoi(need(i)); ++i; }
        else if (a == "--cfar")            { c.cfar_thresh = std::atoi(need(i)); ++i; }
        else if (a == "--streams")        { c.streams = std::atoi(need(i)); ++i; }
        else if (a == "--db-lo")          { c.db_lo = std::atof(need(i)); ++i; }
        else if (a == "--db-hi")          { c.db_hi = std::atof(need(i)); ++i; }
        else if (a == "--conf")           { c.conf = std::atof(need(i)); ++i; }
        else if (a == "--nms-iou")        { c.nms_iou = std::atof(need(i)); ++i; }
        else if (a == "--max-det")        { c.max_det = std::atoi(need(i)); ++i; }
        else if (a == "--buffer-m")       { c.buffer_m = std::atof(need(i)); ++i; }
        else if (a == "--coarse-decim")   { c.coarse_decim = std::atoi(need(i)); ++i; }
        else if (a == "--jpeg-quality")   { c.jpeg_quality = std::atoi(need(i)); ++i; }
        else if (a == "--box-thickness")  { c.box_thickness = std::atoi(need(i)); ++i; }
        else if (a == "--workspace-mb")   { c.workspace_mb = std::atoll(need(i)); ++i; }
        else if (a == "-h" || a == "--help") { usage(argv[0]); std::exit(0); }
        else { throw std::runtime_error("unknown option " + a); }
    }

    if (c.onnx.empty()) { throw std::runtime_error("no --onnx given"); }
    if (c.tif.empty() == c.l1.safe.empty()) {
        std::fprintf(stderr, c.tif.empty()
            ? "give exactly one of --tif or --safe\n"
            : "--tif and --safe are mutually exclusive: --safe produces the L2 "
              "raster itself\n");
        throw std::runtime_error("give exactly one of --tif or --safe");
    }
    if (c.engine.empty()) c.engine = c.onnx + ".engine";

    if (!c.out_crops.empty()) {
        if (c.crop.size < 64) {
            std::fprintf(stderr, "--crop-size %d is too small; using 1024\n", c.crop.size);
            c.crop.size = 1024;
        }

        if (c.crop.thumb > 0) {
            if (c.crop.thumb >= c.crop.size) {
                c.crop.thumb = 0;
            } else if (c.crop.size % c.crop.thumb != 0) {
                int f = c.crop.size / c.crop.thumb;
                while (f < c.crop.size && c.crop.size % f != 0) ++f;
                const int t = c.crop.size / f;
                std::fprintf(stderr,
                    "[crops] --crop-thumb %d does not divide --crop-size %d; using %d\n",
                    c.crop.thumb, c.crop.size, t);
                c.crop.thumb = t;
            }
        }
    }
    return c;
}
