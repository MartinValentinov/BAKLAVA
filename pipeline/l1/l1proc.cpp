#include "l1proc.hpp"
#include "kernels.cuh"
#include <cuda_fp16.h>
#include "rdgeom.hpp"
#include "../io/safe.hpp"
#include "../io/scene.hpp"
#include "../common/util.hpp"
#include "../common/quantise.cuh"

#include <gdal_priv.h>
#include <gdalwarper.h>
#include <ogr_spatialref.h>
#include <cpl_conv.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <atomic>
#include <thread>
#include <vector>

namespace {

struct Dem {
    std::vector<float> h;
    int w = 0, ht = 0;
    double lon0 = 0, lat0 = 0, dlon = 0, dlat = 0;
    float noData = -32768.0f;
    bool ok() const { return w > 0 && ht > 0; }
};

float sampleGrid(const std::vector<float>& g, int w, int h,
                 double lon0, double lat0, double dlon, double dlat,
                 double lon, double lat) {
    const double dx = (lon - lon0) / dlon, dy = (lat - lat0) / dlat;
    const int ix = int(std::floor(dx)), iy = int(std::floor(dy));
    if (ix < 0 || iy < 0 || ix >= w - 1 || iy >= h - 1) return 0.0f;
    const double tx = dx - ix, ty = dy - iy;
    const double a = g[size_t(iy) * w + ix], b = g[size_t(iy) * w + ix + 1];
    const double c = g[size_t(iy + 1) * w + ix], d = g[size_t(iy + 1) * w + ix + 1];
    return float((1 - ty) * (a + tx * (b - a)) + ty * (c + tx * (d - c)));
}

bool readGeoGrid(const std::string& path, double lonMin, double lonMax,
                 double latMin, double latMax,
                 std::vector<float>& out, int& w, int& h,
                 double& lon0, double& lat0, double& dlon, double& dlat,
                 float& noData, const char* what, bool mustCover = false) {
    GDALDataset* ds = static_cast<GDALDataset*>(GDALOpen(path.c_str(), GA_ReadOnly));
    if (!ds) { std::fprintf(stderr, "[l1] cannot open %s %s\n", what, path.c_str()); return false; }

    GDALDataset* use = ds;
    GDALDataset* warped = nullptr;
    const char* wkt = ds->GetProjectionRef();
    OGRSpatialReference srs, wgs;
    wgs.importFromEPSG(4326);
    wgs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    bool is4326 = false;
    if (wkt && *wkt && srs.importFromWkt(&wkt) == OGRERR_NONE) {
        srs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
        is4326 = srs.IsSame(&wgs);
    }
    if (!is4326) {
        char* wgsWkt = nullptr;
        wgs.exportToWkt(&wgsWkt);
        warped = static_cast<GDALDataset*>(
            GDALAutoCreateWarpedVRT(ds, nullptr, wgsWkt, GRA_Bilinear, 0.0, nullptr));
        CPLFree(wgsWkt);
        if (!warped) {
            std::fprintf(stderr, "[l1] cannot reproject %s to WGS84\n", what);
            GDALClose(ds);
            return false;
        }
        use = warped;
        std::fprintf(stderr, "[l1] %s reprojected to WGS84 on the fly\n", what);
    }

    double gt[6];
    if (use->GetGeoTransform(gt) != CE_None) {
        std::fprintf(stderr, "[l1] %s has no geotransform\n", what);
        if (warped) GDALClose(warped);
        GDALClose(ds);
        return false;
    }

    const int fw = use->GetRasterXSize(), fh = use->GetRasterYSize();
    int x0 = int(std::floor((lonMin - gt[0]) / gt[1])) - 2;
    int x1 = int(std::ceil((lonMax - gt[0]) / gt[1])) + 2;
    int y0 = int(std::floor((latMax - gt[3]) / gt[5])) - 2;
    int y1 = int(std::ceil((latMin - gt[3]) / gt[5])) + 2;
    if (mustCover && (x0 < 0 || y0 < 0 || x1 > fw || y1 > fh)) {
        std::fprintf(stderr,
            "[l1] ERROR: %s %s does not cover this scene.\n"
            "     scene needs lon [%.3f, %.3f] lat [%.3f, %.3f]; the grid holds\n"
            "     lon [%.3f, %.3f] lat [%.3f, %.3f].\n"
            "     Reading it anyway would silently geocode against the wrong\n"
            "     terrain. Point DEM_NAME at a grid that covers this scene\n"
            "     (dem.tif is the full-coverage mosaic).\n",
            what, path.c_str(), lonMin, lonMax, latMin, latMax,
            gt[0], gt[0] + fw * gt[1], gt[3] + fh * gt[5], gt[3]);
        if (warped) GDALClose(warped);
        GDALClose(ds);
        return false;
    }
    x0 = std::max(0, std::min(x0, fw - 1)); x1 = std::max(x0 + 1, std::min(x1, fw));
    y0 = std::max(0, std::min(y0, fh - 1)); y1 = std::max(y0 + 1, std::min(y1, fh));

    w = x1 - x0; h = y1 - y0;
    lon0 = gt[0] + (x0 + 0.5) * gt[1];
    lat0 = gt[3] + (y0 + 0.5) * gt[5];
    dlon = gt[1]; dlat = gt[5];

    out.resize(size_t(w) * h);
    int hasND = 0;
    noData = float(use->GetRasterBand(1)->GetNoDataValue(&hasND));
    if (!hasND) noData = -32768.0f;

    const CPLErr e = use->GetRasterBand(1)->RasterIO(
        GF_Read, x0, y0, w, h, out.data(), w, h, GDT_Float32, 0, 0, nullptr);
    if (warped) GDALClose(warped);
    GDALClose(ds);
    if (e != CE_None) { std::fprintf(stderr, "[l1] %s read failed\n", what); return false; }

    std::fprintf(stderr, "[l1] %s %d x %d over lon [%.3f, %.3f] lat [%.3f, %.3f]\n",
                 what, w, h, lon0, lon0 + (w - 1) * dlon, lat0 + (h - 1) * dlat, lat0);
    return true;
}

template <typename T>
T* upload(const std::vector<T>& v) {
    if (v.empty()) return nullptr;
    T* d = nullptr;
    CUDA_CHECK(cudaMalloc(&d, v.size() * sizeof(T)));
    CUDA_CHECK(cudaMemcpy(d, v.data(), v.size() * sizeof(T), cudaMemcpyHostToDevice));
    return d;
}

void lineBrackets(const std::vector<int>& rows, int nl,
                  std::vector<int>& idx, std::vector<float>& wgt) {
    idx.resize(size_t(nl));
    wgt.resize(size_t(nl));
    const int n = int(rows.size());
    for (int y = 0; y < nl; ++y) {
        int i = 0;
        while (i + 2 < n && rows[size_t(i + 1)] < y) ++i;
        const int j = std::min(i + 1, n - 1);
        const double a = rows[size_t(i)], b = rows[size_t(j)];
        idx[size_t(y)] = i;
        wgt[size_t(y)] = float(b > a ? std::min(1.0, std::max(0.0, (y - a) / (b - a))) : 0.0);
    }
}

}

bool runL1ToL2(const L1Config& cfg, Scene& scene,
               const std::function<void()>& onGeometryReady,
               L1Quant* quant) {
    Timer tAll("L1->L2 total");

    SafeProduct sp;
    {
        Timer t("  SAFE metadata parse");
        if (!sp.open(cfg.safe, cfg.pol)) return false;
        t.report();
    }
    if (sp.nCal() < 2) { std::fprintf(stderr, "[l1] need at least 2 calibration rows\n"); return false; }

    double lonMin = 1e30, lonMax = -1e30, latMin = 1e30, latMax = -1e30;
    for (const SafeGcp& g : sp.gcps) {
        lonMin = std::min(lonMin, g.lon); lonMax = std::max(lonMax, g.lon);
        latMin = std::min(latMin, g.lat); latMax = std::max(latMax, g.lat);
    }
    const double lonC = 0.5 * (lonMin + lonMax), latC = 0.5 * (latMin + latMax);

    int epsg = cfg.epsg;
    if (epsg == 0) {
        const int zone = int(std::floor((lonC + 180.0) / 6.0)) + 1;
        epsg = (latC >= 0 ? 32600 : 32700) + zone;
    }
    OGRSpatialReference outSrs;
    if (outSrs.importFromEPSG(epsg) != OGRERR_NONE) {
        std::fprintf(stderr, "[l1] bad EPSG %d\n", epsg);
        return false;
    }
    outSrs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);

    OGRSpatialReference wgs;
    wgs.importFromEPSG(4326);
    wgs.SetAxisMappingStrategy(OAMS_TRADITIONAL_GIS_ORDER);
    OGRCoordinateTransformation* toMap = OGRCreateCoordinateTransformation(&wgs, &outSrs);
    OGRCoordinateTransformation* toGeo = OGRCreateCoordinateTransformation(&outSrs, &wgs);
    if (!toMap || !toGeo) { std::fprintf(stderr, "[l1] cannot build CRS transforms\n"); return false; }

    double eMin = 1e30, eMax = -1e30, nMin = 1e30, nMax = -1e30;
    for (const SafeGcp& g : sp.gcps) {
        double x = g.lon, y = g.lat;
        if (!toMap->Transform(1, &x, &y)) continue;
        eMin = std::min(eMin, x); eMax = std::max(eMax, x);
        nMin = std::min(nMin, y); nMax = std::max(nMax, y);
    }
    const double pad = 1000.0;
    eMin -= pad; eMax += pad; nMin -= pad; nMax += pad;

    const double px = cfg.pixel_spacing;
    const int W = int(std::ceil((eMax - eMin) / px));
    const int H = int(std::ceil((nMax - nMin) / px));
    double gt[6] = {eMin, px, 0.0, nMax, 0.0, -px};
    std::fprintf(stderr, "[l1] output EPSG:%d  %d x %d at %.1f m\n", epsg, W, H, px);

    OrbitTable tab;
    const double tRelStart = -2.0, tRelEnd = (sp.t1 - sp.t0) + 2.0;

    Vec3 origin, dummyN;
    geodeticToEcef(latC, lonC, 0.0, origin, dummyN);
    {
        Timer t("  orbit table");
        tab.build(sp, origin, tRelStart, tRelEnd, 1e-3);
        t.report();
    }

    Dem dem;
    auto loadDem = [&]() -> bool {
    if (cfg.dem.empty()) {
        std::fprintf(stderr, "[l1] no --dem given; geocoding on the ellipsoid. "
                             "Water is unaffected, land will be displaced by the "
                             "terrain height divided by tan(incidence).\n");
    } else {
        {
            Timer t("  DEM read");
            if (!readGeoGrid(cfg.dem, lonMin - 0.2, lonMax + 0.2, latMin - 0.2, latMax + 0.2,
                             dem.h, dem.w, dem.ht, dem.lon0, dem.lat0, dem.dlon, dem.dlat,
                             dem.noData, "DEM", true))
                return false;
            for (float& v : dem.h) if (v == dem.noData) v = 0.0f;
            t.report();
        }

        if (!cfg.geoid.empty()) {
            Timer tGeoid("  geoid read + apply");
            std::vector<float> gg; int gw = 0, gh = 0;
            double glon0, glat0, gdlon, gdlat; float gnd;
            if (!readGeoGrid(cfg.geoid, lonMin - 1.0, lonMax + 1.0, latMin - 1.0, latMax + 1.0,
                             gg, gw, gh, glon0, glat0, gdlon, gdlat, gnd, "geoid"))
                return false;

            const unsigned hc = std::thread::hardware_concurrency();
            const int nThreads = int(hc > 1 ? hc : 1);
            std::vector<double> partial(size_t(nThreads), 0.0);
            {
                std::vector<std::thread> pool;
                for (int th = 0; th < nThreads; ++th) {
                    pool.emplace_back([&, th]() {
                        double s = 0.0;
                        for (int y = th; y < dem.ht; y += nThreads) {
                            const double lat = dem.lat0 + y * dem.dlat;
                            for (int x = 0; x < dem.w; ++x) {
                                const double lon = dem.lon0 + x * dem.dlon;
                                const float n = sampleGrid(gg, gw, gh, glon0, glat0,
                                                           gdlon, gdlat, lon, lat);
                                dem.h[size_t(y) * dem.w + x] += n;
                                s += n;
                            }
                        }
                        partial[size_t(th)] = s;
                    });
                }
                for (auto& x : pool) x.join();
            }
            double sum = 0.0;
            for (double s : partial) sum += s;
            std::fprintf(stderr, "[l1] geoid applied, mean undulation %+.2f m\n",
                         sum / (double(dem.w) * dem.ht));
            tGeoid.report();
        } else if (!cfg.no_geoid_ok) {
            std::fprintf(stderr,
                "[l1] ERROR: --dem was given without --geoid.\n"
                "     SRTM and most other DEMs are orthometric (heights above the\n"
                "     geoid), but the range-Doppler solve needs ellipsoidal heights.\n"
                "     In the Black Sea the difference is about +35 m, which is a\n"
                "     ~4 pixel geolocation error -- measured, not estimated.\n"
                "     Pass --geoid with an undulation grid (PROJ ships\n"
                "     us_nga_egm96_15.tif), or --dem-is-ellipsoidal if your DEM\n"
                "     already has ellipsoidal heights (Copernicus DEM does not).\n");
            return false;
        }
    }
    return true;
    };

    const int step = std::max(1, cfg.coarse_step);
    const int cw = W / step + 2, ch = H / step + 2;
    std::vector<float3> cT0(size_t(cw) * ch), cU(size_t(cw) * ch);
    std::vector<float>  cTs(size_t(cw) * ch);
    std::vector<float2> cLL(size_t(cw) * ch);
    auto buildLattice = [&]() {
        Timer t("  coarse lattice");
        const unsigned hc = std::thread::hardware_concurrency();
        const int nThreads = int(hc > 3 ? hc - 3 : 1);
        std::vector<std::thread> pool;
        for (int th = 0; th < nThreads; ++th) {
            pool.emplace_back([&, th]() {
                OGRCoordinateTransformation* tg =
                    OGRCreateCoordinateTransformation(&outSrs, &wgs);
                if (!tg) return;
                for (int j = th; j < ch; j += nThreads) {
                    bool haveSeed = false;
                    double seed = 0.0;
                    for (int i = 0; i < cw; ++i) {
                        double x = gt[0] + (double(i) * step + 0.5) * gt[1];
                        double y = gt[3] + (double(j) * step + 0.5) * gt[5];
                        double lon = x, lat = y;
                        tg->Transform(1, &lon, &lat);

                        Vec3 p, n;
                        geodeticToEcef(lat, lon, 0.0, p, n);
                        p = p - origin;

                        const double tt = zeroDoppler(tab, p, seed, haveSeed);
                        seed = tt; haveSeed = true;

                        const size_t k = size_t(j) * cw + i;
                        cT0[k] = make_float3(float(p.x), float(p.y), float(p.z));
                        cU[k]  = make_float3(float(n.x), float(n.y), float(n.z));
                        cTs[k] = float(tt);
                        cLL[k] = make_float2(float(lon), float(lat));
                    }
                }
                OCTDestroyCoordinateTransformation(
                    reinterpret_cast<OGRCoordinateTransformationH>(tg));
            });
        }
        for (auto& x : pool) x.join();
        t.report();
    };

    Timer tSrgr("  slant-range tables");
    std::vector<SrgrDev> srgr;
    for (const SrgrSet& s : sp.srgr) {
        SrgrDev d{};
        d.t = s.t - sp.t0;
        d.sr0 = s.sr0; d.gr0 = s.gr0; d.nc = s.nc;
        for (int i = 0; i < s.nc; ++i) d.c[i] = s.c[i];
        srgr.push_back(d);
    }
    if (srgr.size() < 2) srgr.push_back(srgr.front());
    std::vector<int> srgrIdx(size_t(tab.n));
    for (int i = 0; i < tab.n; ++i) {
        const double t = tab.t0 + i * tab.dt;
        int k = 0;
        while (k + 2 < int(srgr.size()) && srgr[size_t(k + 1)].t < t) ++k;
        srgrIdx[size_t(i)] = k;
    }
    tSrgr.report();

    uint16_t* dnHost = nullptr;
    uint16_t* dnDev = nullptr;
    __half* dSigma = nullptr;
    auto loadMeasurement = [&]() -> bool {
    {

        Timer t("  measurement read + calibrate");
        GDALDataset* ds = static_cast<GDALDataset*>(GDALOpen(sp.measurement.c_str(), GA_ReadOnly));
        if (!ds) { std::fprintf(stderr, "[l1] cannot open %s\n", sp.measurement.c_str()); return false; }
        if (ds->GetRasterXSize() != sp.ns || ds->GetRasterYSize() != sp.nl) {
            std::fprintf(stderr, "[l1] measurement is %dx%d but annotation says %dx%d\n",
                         ds->GetRasterXSize(), ds->GetRasterYSize(), sp.ns, sp.nl);
            GDALClose(ds);
            return false;
        }
        const size_t n = size_t(sp.ns) * sp.nl;
        CUDA_CHECK(cudaHostAlloc(reinterpret_cast<void**>(&dnHost), n * sizeof(uint16_t),
                                 cudaHostAllocMapped | cudaHostAllocPortable));
        CUDA_CHECK(cudaHostGetDevicePointer(reinterpret_cast<void**>(&dnDev), dnHost, 0));
        CUDA_CHECK(cudaMalloc(&dSigma, n * sizeof(__half)));

        std::vector<int> calIdx; std::vector<float> calW;
        lineBrackets(sp.calLines, sp.nl, calIdx, calW);
        std::vector<int> noiIdx; std::vector<float> noiW;
        const bool haveNoise = sp.nNoise() >= 2;
        if (haveNoise) lineBrackets(sp.noiseLines, sp.nl, noiIdx, noiW);

        std::vector<int> swathOf(size_t(sp.ns), -1);
        for (int s = 0; s < sp.nSwath(); ++s)
            for (int x = std::max(0, sp.azFirstSample[size_t(s)]);
                 x <= std::min(sp.ns - 1, sp.azLastSample[size_t(s)]); ++x)
                swathOf[size_t(x)] = s;

        float* dCal   = upload(sp.calRows);
        int*   dCalI  = upload(calIdx);
        float* dCalW  = upload(calW);
        float* dNoi   = haveNoise ? upload(sp.noiseRows) : nullptr;
        int*   dNoiI  = haveNoise ? upload(noiIdx) : nullptr;
        float* dNoiW  = haveNoise ? upload(noiW) : nullptr;
        float* dAz    = sp.nSwath() ? upload(sp.azNoise) : nullptr;
        int*   dSw    = upload(swathOf);

        cudaStream_t st;
        CUDA_CHECK(cudaStreamCreateWithFlags(&st, cudaStreamNonBlocking));

        const CPLErr e = ds->GetRasterBand(1)->RasterIO(
            GF_Read, 0, 0, sp.ns, sp.nl, dnHost, sp.ns, sp.nl, GDT_UInt16, 0, 0, nullptr);
        if (e == CE_None)
            launch_calibrate(dnDev, sp.ns, sp.nl, dCal, dCalI, dCalW,
                             dNoi, dNoiI, dNoiW, dAz, dSw, sp.nl, dSigma,
                             0, sp.nl, cfg.noise_scale, st);
        GDALClose(ds);
        CUDA_CHECK(cudaStreamSynchronize(st));
        CUDA_CHECK(cudaStreamDestroy(st));
        if (e != CE_None) { std::fprintf(stderr, "[l1] measurement read failed\n"); return false; }

        for (void* p : {(void*)dCal, (void*)dCalI, (void*)dCalW, (void*)dNoi,
                        (void*)dNoiI, (void*)dNoiW, (void*)dAz, (void*)dSw})
            if (p) CUDA_CHECK(cudaFree(p));
        CUDA_CHECK(cudaFreeHost(dnHost));
        t.report();
    }
    return true;
    };

    char* outWkt = nullptr;
    outSrs.exportToWkt(&outWkt);

    const bool fuseQuant = quant && quant->enabled;
    const bool wantFloat = !cfg.out_l2.empty() || !fuseQuant;
    if (fuseQuant) {
        quant->decim = std::max(1, quant->decim);
        quant->cw = (W + quant->decim - 1) / quant->decim;
        quant->ch = (H + quant->decim - 1) / quant->decim;
    }

    if (!scene.setGeometry(W, H, gt, outWkt, sp.basename + "_" + sp.polarisation,
                           sp.startISO, sp.stopISO)) {
        CPLFree(outWkt);
        return false;
    }
    CPLFree(outWkt);
    if (onGeometryReady) onGeometryReady();

    std::atomic<bool> demOk{true}, measOk{true}, rastOk{true};
    Timer tPar("  parallel load (4 lanes)");
    std::thread thDem([&] { demOk.store(loadDem()); });
    std::thread thMeas([&] { measOk.store(loadMeasurement()); });
    std::thread thRast([&] {
        Timer t("  output raster alloc");
        bool ok = scene.allocateRaster(wantFloat);
        if (ok && fuseQuant) {
            if (quant->hostVisible) {
                CUDA_CHECK(cudaHostAlloc(reinterpret_cast<void**>(&quant->grayHost),
                                         size_t(W) * H,
                                         cudaHostAllocMapped | cudaHostAllocPortable));
                CUDA_CHECK(cudaHostGetDevicePointer(
                    reinterpret_cast<void**>(&quant->grayDev), quant->grayHost, 0));
            } else {
                CUDA_CHECK(cudaMalloc(reinterpret_cast<void**>(&quant->grayDev),
                                      size_t(W) * H));
            }
        }
        rastOk.store(ok);
        t.report();
    });
    buildLattice();
    thDem.join();
    thMeas.join();
    thRast.join();
    tPar.report();
    if (!demOk.load() || !measOk.load() || !rastOk.load()) return false;

    const int lutN = 16384;
    std::vector<float> lutTab, lutSr0, lutGr0, lutT0, lutInvDt;
    double lutDdMin = 0.0, lutStep = 1.0;
    {
        Timer t("  srgr table");
        auto poly = [&](const SrgrDev& s, double dd) {
            double g = 0.0, pw = 1.0;
            for (int k = 0; k < s.nc; ++k) { g += s.c[k] * pw; pw *= dd; }
            return g;
        };

        const double grTarget = 1.25 * double(sp.ns) * sp.rgSpacing;
        double lo = 0.0, hi = 1.0;
        while (poly(srgr[0], hi) < grTarget && hi < 1e8) hi *= 2.0;
        for (int it = 0; it < 60; ++it) {
            const double mid = 0.5 * (lo + hi);
            (poly(srgr[0], mid) < grTarget ? lo : hi) = mid;
        }
        const double ddMax = hi;
        const double ddMin = -0.05 * ddMax;
        const double step = (ddMax - ddMin) / double(lutN - 1);
        lutDdMin = ddMin; lutStep = step;

        const int nSet = int(srgr.size());
        lutTab.resize(size_t(nSet) * lutN);
        lutSr0.resize(size_t(nSet)); lutGr0.resize(size_t(nSet));
        lutT0.resize(size_t(nSet));  lutInvDt.resize(size_t(nSet));
        for (int i = 0; i < nSet; ++i) {
            for (int j = 0; j < lutN; ++j)
                lutTab[size_t(i) * lutN + j] = float(poly(srgr[size_t(i)], ddMin + j * step));
            lutSr0[size_t(i)] = float(srgr[size_t(i)].sr0);
            lutGr0[size_t(i)] = float(srgr[size_t(i)].gr0);
            lutT0[size_t(i)]  = float(srgr[size_t(i)].t);
        }
        for (int i = 0; i < nSet; ++i) {
            const double dt = (i + 1 < nSet) ? srgr[size_t(i + 1)].t - srgr[size_t(i)].t : 0.0;
            lutInvDt[size_t(i)] = float(dt > 0.0 ? 1.0 / dt : 0.0);
        }
        std::fprintf(stderr, "[l1] srgr table %d sets x %d over dd [%.0f, %.0f] m\n",
                     nSet, lutN, ddMin, ddMax);
        t.report();
    }

    {
        Timer t("  range-doppler geocode");
        RdParams prm{};
        prm.W = W; prm.H = H;
        prm.cw = cw; prm.ch = ch; prm.cstep = step;
        prm.ns = sp.ns; prm.nl = sp.nl;
        prm.dtAz = float(sp.dtAz);
        prm.rgSpacing = float(sp.rgSpacing);
        prm.tabT0 = float(tab.t0); prm.tabDt = float(tab.dt); prm.tabN = tab.n;
        prm.demW = dem.w; prm.demH = dem.ht;
        prm.demLon0 = float(dem.lon0); prm.demLat0 = float(dem.lat0);
        prm.demDLon = float(dem.dlon); prm.demDLat = float(dem.dlat);
        prm.demNoData = dem.noData;

        std::vector<float3> hP(size_t(tab.n)), hV(size_t(tab.n)), hA(size_t(tab.n));
        for (int i = 0; i < tab.n; ++i) {
            hP[size_t(i)] = make_float3(float(tab.p[size_t(i)].x), float(tab.p[size_t(i)].y),
                                        float(tab.p[size_t(i)].z));
            hV[size_t(i)] = make_float3(float(tab.v[size_t(i)].x), float(tab.v[size_t(i)].y),
                                        float(tab.v[size_t(i)].z));
            hA[size_t(i)] = make_float3(float(tab.a[size_t(i)].x), float(tab.a[size_t(i)].y),
                                        float(tab.a[size_t(i)].z));
        }

        float3* dT0 = upload(cT0);  float3* dU  = upload(cU);
        float*  dTs = upload(cTs);  float2* dLL = upload(cLL);
        float*  dDem = dem.ok() ? upload(dem.h) : nullptr;
        float3* dP = upload(hP);    float3* dV = upload(hV);   float3* dA = upload(hA);
        SrgrDev* dS = upload(srgr); int* dSI = upload(srgrIdx);

        SrgrLut lut{};
        float* dLutTab   = upload(lutTab);
        float* dLutSr0   = upload(lutSr0);
        float* dLutGr0   = upload(lutGr0);
        float* dLutT0    = upload(lutT0);
        float* dLutInvDt = upload(lutInvDt);
        lut.tab = dLutTab; lut.sr0 = dLutSr0; lut.gr0 = dLutGr0;
        lut.t0 = dLutT0;   lut.invDt = dLutInvDt;
        lut.nSet = int(srgr.size()); lut.n = lutN;
        lut.ddMin = float(lutDdMin);
        lut.invStep = float(1.0 / lutStep);

        unsigned char* dSwath = nullptr;
        CUDA_CHECK(cudaMalloc(&dSwath, size_t(cw) * ch));
        prm.swath = nullptr;
        launch_swath_mask(prm, dT0, dU, dTs, dLL, dDem, dP, dV, dA, dS, dSI,
                          dSwath, 0);
        CUDA_CHECK(cudaDeviceSynchronize());
        prm.swath = dSwath;

        RdQuant q;
        unsigned char* dMask = nullptr;
        unsigned int*  dMax  = nullptr;
        unsigned int*  dSum  = nullptr;
        const size_t nCells = fuseQuant ? size_t(quant->cw) * quant->ch : 0;
        if (fuseQuant) {
            CUDA_CHECK(cudaMalloc(&dMask, nCells));
            CUDA_CHECK(cudaMemset(dMask, 0, nCells));
            CUDA_CHECK(cudaMalloc(&dMax, nCells * sizeof(unsigned int)));
            CUDA_CHECK(cudaMemset(dMax, 0, nCells * sizeof(unsigned int)));
            CUDA_CHECK(cudaMalloc(&dSum, nCells * sizeof(unsigned int)));
            CUDA_CHECK(cudaMemset(dSum, 0, nCells * sizeof(unsigned int)));
            q.gray     = quant->grayDev;
            q.dataMask = dMask;
            q.maxGrid  = dMax;
            q.sumGrid  = dSum;
            q.cw       = quant->cw;
            q.decim    = quant->decim;
            q.shift    = quant::decim_shift(quant->decim);
            q.lo       = quant->db_lo;
            q.invSpan  = quant::inv_span(quant->db_lo, quant->db_hi);
        }

        launch_rd_geocode(prm, dT0, dU, dTs, dLL, dDem, dP, dV, dA, dS, dSI,
                          dSigma, scene.device(), q, lut, 0);
        CUDA_CHECK(cudaDeviceSynchronize());

        if (fuseQuant) {
            quant->dataMask.resize(nCells);
            quant->maxGrid.resize(nCells);
            quant->sumGrid.resize(nCells);
            CUDA_CHECK(cudaMemcpy(quant->dataMask.data(), dMask, nCells,
                                  cudaMemcpyDeviceToHost));
            CUDA_CHECK(cudaMemcpy(quant->maxGrid.data(), dMax,
                                  nCells * sizeof(unsigned int),
                                  cudaMemcpyDeviceToHost));
            CUDA_CHECK(cudaMemcpy(quant->sumGrid.data(), dSum,
                                  nCells * sizeof(unsigned int),
                                  cudaMemcpyDeviceToHost));
            CUDA_CHECK(cudaFree(dMask));
            CUDA_CHECK(cudaFree(dMax));
            CUDA_CHECK(cudaFree(dSum));
        }

        CUDA_CHECK(cudaFree(dSwath));
        for (void* p : {(void*)dT0, (void*)dU, (void*)dTs, (void*)dLL, (void*)dDem,
                        (void*)dP, (void*)dV, (void*)dA, (void*)dS, (void*)dSI,
                        (void*)dLutTab, (void*)dLutSr0, (void*)dLutGr0,
                        (void*)dLutT0, (void*)dLutInvDt})
            if (p) CUDA_CHECK(cudaFree(p));
        CUDA_CHECK(cudaFree(dSigma));
        t.report();
    }

    OCTDestroyCoordinateTransformation(reinterpret_cast<OGRCoordinateTransformationH>(toMap));
    OCTDestroyCoordinateTransformation(reinterpret_cast<OGRCoordinateTransformationH>(toGeo));

    if (!cfg.out_l2.empty()) {
        Timer t("  write L2 GeoTIFF");
        GDALDriver* drv = GetGDALDriverManager()->GetDriverByName("GTiff");
        if (!drv) {
            std::fprintf(stderr, "[l1] no GTiff driver; skipping --out-l2\n");
        } else {
            char** opts = nullptr;
            opts = CSLSetNameValue(opts, "BIGTIFF", "YES");
            opts = CSLSetNameValue(opts, "TILED", "NO");
            opts = CSLSetNameValue(opts, "NUM_THREADS", "ALL_CPUS");
            GDALDataset* od = drv->Create(cfg.out_l2.c_str(), W, H, 1, GDT_Float32, opts);
            CSLDestroy(opts);
            if (od) {
                od->SetGeoTransform(gt);
                char* wkt2 = nullptr;
                outSrs.exportToWkt(&wkt2);
                od->SetProjection(wkt2);
                CPLFree(wkt2);
                const CPLErr we = od->GetRasterBand(1)->RasterIO(
                    GF_Write, 0, 0, W, H, scene.host(), W, H, GDT_Float32, 0, 0, nullptr);
                GDALClose(od);
                if (we == CE_None)
                    std::fprintf(stderr, "[l1] wrote %s\n", cfg.out_l2.c_str());
                else
                    std::fprintf(stderr, "[l1] WARNING --out-l2 write failed; the "
                                         "detection run itself is unaffected\n");
            }
        }
        t.report();
    }

    tAll.report();
    return true;
}
