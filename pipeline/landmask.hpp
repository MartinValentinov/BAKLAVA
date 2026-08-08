#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

class OGRGeometry;
class Scene;

// GSHHG-derived exclusion zone.
//
// Every GSHHG level (L1 land, L2 lakes, L3 islands-in-lakes, L4 ponds,
// L5/L6 Antarctic) is unioned into a single "not open sea" region and buffered
// outward by `buffer_m`. A detection whose CENTRE falls inside is dropped.
//
// Two representations are built:
//   * exact vector geometry, used to filter final detections (no raster error)
//   * a coarse, eroded raster, used only to skip tiles that are entirely land
//     before inference -- an accelerator that cannot change the output
class LandMask {
public:
    ~LandMask();

    // shp = GSHHS_f_L1.shp ... GSHHS_f_L6.shp (any subset)
    bool build(const std::vector<std::string>& shp, const Scene& scene,
               double buffer_m, int coarse_decim);

    // exact test, scene CRS map coordinates
    bool isLand(double x, double y) const;

    // conservative: true only if the whole tile is certainly inside the
    // exclusion zone, so skipping it cannot discard a sea detection
    bool tileFullyLand(int x0, int y0, int tile) const;

    size_t polygonCount() const { return geoms_.size(); }
    bool   ready() const { return ready_; }

private:
    struct Env { double minx, miny, maxx, maxy; };

    std::vector<OGRGeometry*> geoms_;
    std::vector<Env>          envs_;

    // coarse raster in scene pixel space, decimated by decim_
    std::vector<uint8_t> coarse_;   // 1 = certainly land
    int cw_ = 0, ch_ = 0, decim_ = 8;

    bool ready_ = false;

    void rasterizeCoarse(const Scene& scene);
    void erode();
};
