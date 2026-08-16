#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <memory>

class OGRGeometry;
struct _OGRPreparedGeometry;
class Scene;

class LandMask {
public:
    ~LandMask();

    bool build(const std::vector<std::string>& shp, const Scene& scene,
               double buffer_m, int coarse_decim);

    bool isLand(double x, double y) const;

    bool tileFullyLand(int x0, int y0, int tile) const;

    size_t polygonCount() const { return geoms_.size(); }
    bool   ready() const { return ready_; }

    const std::vector<uint8_t>& coarseRaw() const { return coarseRaw_; }
    int coarseW() const { return cw_; }
    int coarseH() const { return ch_; }
    int decim()   const { return decim_; }

private:
    struct Env { double minx, miny, maxx, maxy; };

    std::vector<OGRGeometry*> geoms_;
    std::vector<Env>          envs_;
    std::vector<_OGRPreparedGeometry*> prepared_;

    std::vector<uint8_t> coarse_;
    std::vector<uint8_t> coarseRaw_;
    int cw_ = 0, ch_ = 0, decim_ = 8;

    bool ready_ = false;

    void rasterizeCoarse(const Scene& scene);
    void erode();
};
