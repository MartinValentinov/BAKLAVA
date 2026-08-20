#include "landmask.hpp"
#include "scene.hpp"
#include "../common/util.hpp"

#include <gdal_priv.h>
#include <gdal_alg.h>
#include <gdal_version.h>
#include <ogrsf_frmts.h>
#include <ogr_geometry.h>
#include <ogr_spatialref.h>

#include <algorithm>
#include <cmath>

#if defined(GDAL_VERSION_NUM) && GDAL_VERSION_NUM >= 3030000
#define HAVE_PREPARED_GEOMETRY 1
#else
#define HAVE_PREPARED_GEOMETRY 0
#endif

LandMask::~LandMask() {
#if HAVE_PREPARED_GEOMETRY
    for (auto* p : prepared_) if (p) OGRDestroyPreparedGeometry(p);
#endif
    for (auto* g : geoms_) OGRGeometryFactory::destroyGeometry(g);
}

bool LandMask::build(const std::vector<std::string>& shp, const Scene& scene,
                     double buffer_m, int coarse_decim) {
    decim_ = std::max(1, coarse_decim);
    if (shp.empty()) {
        std::fprintf(stderr, "[land] no GSHHG shapefiles given; land filtering disabled\n");
        return false;
    }

    double minx, miny, maxx, maxy;
    scene.bboxMap(minx, miny, maxx, maxy);
    const double pad = buffer_m + 1000.0;
    minx -= pad; miny -= pad; maxx += pad; maxy += pad;

    OGRSpatialReference wgs;
    wgs.importFromEPSG(4326);
    wgs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRCoordinateTransformation* toWgs =
        OGRCreateCoordinateTransformation(scene.srs(), &wgs);
    if (!toWgs) { std::fprintf(stderr, "[land] no transform to WGS84\n"); return false; }

    double bx[4] = {minx, maxx, maxx, minx};
    double by[4] = {miny, miny, maxy, maxy};
    toWgs->Transform(4, bx, by);
    const double wminx = *std::min_element(bx, bx + 4), wmaxx = *std::max_element(bx, bx + 4);
    const double wminy = *std::min_element(by, by + 4), wmaxy = *std::max_element(by, by + 4);
    OCTDestroyCoordinateTransformation(
        reinterpret_cast<OGRCoordinateTransformationH>(toWgs));

    OGRLinearRing ring;
    ring.addPoint(minx, miny); ring.addPoint(maxx, miny);
    ring.addPoint(maxx, maxy); ring.addPoint(minx, maxy); ring.addPoint(minx, miny);
    OGRPolygon clipRect;
    clipRect.addRing(&ring);

    OGRLinearRing ringWgs;
    ringWgs.addPoint(wminx, wminy); ringWgs.addPoint(wmaxx, wminy);
    ringWgs.addPoint(wmaxx, wmaxy); ringWgs.addPoint(wminx, wmaxy); ringWgs.addPoint(wminx, wminy);
    OGRPolygon clipRectWgs;
    clipRectWgs.addRing(&ringWgs);

    for (const auto& path : shp) {
        GDALDataset* ds = static_cast<GDALDataset*>(
            GDALOpenEx(path.c_str(), GDAL_OF_VECTOR | GDAL_OF_READONLY,
                       nullptr, nullptr, nullptr));
        if (!ds) { std::fprintf(stderr, "[land] cannot open %s\n", path.c_str()); continue; }

        OGRLayer* layer = ds->GetLayer(0);
        if (!layer) { GDALClose(ds); continue; }

        layer->SetSpatialFilterRect(wminx, wminy, wmaxx, wmaxy);

        auto* lsrs = layer->GetSpatialRef();
        OGRCoordinateTransformation* toScene = nullptr;
        if (lsrs) {
            OGRSpatialReference src(*lsrs);
            src.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
            toScene = OGRCreateCoordinateTransformation(&src, scene.srs());
        }

        layer->ResetReading();
        OGRFeature* f = nullptr;
        int kept = 0;
        while ((f = layer->GetNextFeature()) != nullptr) {
            OGRGeometry* g = f->GetGeometryRef();
            if (g) {
                OGRGeometry* cWgs = g->Intersection(&clipRectWgs);
                if (!cWgs || cWgs->IsEmpty()) {
                    if (cWgs) OGRGeometryFactory::destroyGeometry(cWgs);
                    OGRFeature::DestroyFeature(f);
                    continue;
                }

                if (toScene && cWgs->transform(toScene) != OGRERR_NONE) {
                    std::fprintf(stderr, "[land] reprojection failed for feature %lld, skipping\n",
                                 static_cast<long long>(f->GetFID()));
                    OGRGeometryFactory::destroyGeometry(cWgs);
                    OGRFeature::DestroyFeature(f);
                    continue;
                }

                OGRGeometry* clipped = cWgs->Intersection(&clipRect);
                OGRGeometryFactory::destroyGeometry(cWgs);

                if (clipped && !clipped->IsEmpty()) {
                    OGRGeometry* buf = (buffer_m > 0.0)
                        ? clipped->Buffer(buffer_m, 8)
                        : clipped->clone();
                    OGRGeometryFactory::destroyGeometry(clipped);
                    if (buf && !buf->IsEmpty()) {
                        OGREnvelope e;
                        buf->getEnvelope(&e);
                        geoms_.push_back(buf);
                        envs_.push_back({e.MinX, e.MinY, e.MaxX, e.MaxY});
                        ++kept;
                    } else if (buf) {
                        OGRGeometryFactory::destroyGeometry(buf);
                    }
                } else if (clipped) {
                    OGRGeometryFactory::destroyGeometry(clipped);
                }
            }
            OGRFeature::DestroyFeature(f);
        }
        if (toScene) OCTDestroyCoordinateTransformation(
            reinterpret_cast<OGRCoordinateTransformationH>(toScene));
        GDALClose(ds);
        std::fprintf(stderr, "[land] %s -> %d polygons in scene\n", path.c_str(), kept);
    }

    if (geoms_.empty()) {
        std::fprintf(stderr, "[land] no land polygons intersect this scene\n");
        ready_ = true;
        cw_ = ch_ = 0;
        return true;
    }

#if HAVE_PREPARED_GEOMETRY
    prepared_.reserve(geoms_.size());
    for (auto* g : geoms_) prepared_.push_back(OGRCreatePreparedGeometry(g));
#endif

    rasterizeCoarse(scene);
    erode();
    ready_ = true;
    return true;
}

bool LandMask::isLand(double x, double y) const {
    if (geoms_.empty()) return false;
    OGRPoint p(x, y);
    for (size_t i = 0; i < geoms_.size(); ++i) {
        const Env& e = envs_[i];
        if (x < e.minx || x > e.maxx || y < e.miny || y > e.maxy) continue;
#if HAVE_PREPARED_GEOMETRY
        if (i < prepared_.size() && prepared_[i]) {
            if (OGRPreparedGeometryContains(prepared_[i], &p)) return true;
            continue;
        }
#endif
        if (geoms_[i]->Contains(&p)) return true;
    }
    return false;
}

void LandMask::rasterizeCoarse(const Scene& scene) {
    cw_ = (scene.width()  + decim_ - 1) / decim_;
    ch_ = (scene.height() + decim_ - 1) / decim_;
    coarse_.assign(size_t(cw_) * ch_, 0);

    GDALDriver* memDrv = GetGDALDriverManager()->GetDriverByName("MEM");
    if (!memDrv) return;
    GDALDataset* ds = memDrv->Create("", cw_, ch_, 1, GDT_Byte, nullptr);
    if (!ds) return;

    double gt[6] = {
        scene.gt(0), scene.gt(1) * decim_, scene.gt(2) * decim_,
        scene.gt(3), scene.gt(4) * decim_, scene.gt(5) * decim_
    };
    ds->SetGeoTransform(gt);

    std::vector<OGRGeometryH> hs;
    hs.reserve(geoms_.size());
    for (auto* g : geoms_) hs.push_back(reinterpret_cast<OGRGeometryH>(g));

    std::vector<double> burn(geoms_.size(), 1.0);
    int band = 1;
    char** opts = nullptr;
    opts = CSLSetNameValue(opts, "ALL_TOUCHED", "FALSE");

    GDALRasterizeGeometries(ds, 1, &band,
                            static_cast<int>(hs.size()), hs.data(),
                            nullptr, nullptr, burn.data(), opts,
                            nullptr, nullptr);
    CSLDestroy(opts);

    ds->GetRasterBand(1)->RasterIO(GF_Read, 0, 0, cw_, ch_,
                                   coarse_.data(), cw_, ch_, GDT_Byte, 0, 0, nullptr);
    GDALClose(ds);
}

void LandMask::erode() {
    if (coarse_.empty()) return;
    coarseRaw_ = coarse_;
    std::vector<uint8_t> out(coarse_.size(), 0);
    for (int y = 1; y < ch_ - 1; ++y) {
        for (int x = 1; x < cw_ - 1; ++x) {
            uint8_t v = 1;
            for (int dy = -1; dy <= 1 && v; ++dy)
                for (int dx = -1; dx <= 1 && v; ++dx)
                    if (!coarse_[size_t(y + dy) * cw_ + (x + dx)]) v = 0;
            out[size_t(y) * cw_ + x] = v;
        }
    }
    coarse_.swap(out);
}

bool LandMask::tileFullyLand(int x0, int y0, int tile) const {
    if (coarse_.empty()) return false;
    const int cx0 = x0 / decim_;
    const int cy0 = y0 / decim_;
    const int cx1 = (x0 + tile - 1) / decim_;
    const int cy1 = (y0 + tile - 1) / decim_;
    if (cx0 < 0 || cy0 < 0 || cx1 >= cw_ || cy1 >= ch_) return false;
    for (int y = cy0; y <= cy1; ++y)
        for (int x = cx0; x <= cx1; ++x)
            if (!coarse_[size_t(y) * cw_ + x]) return false;
    return true;
}
