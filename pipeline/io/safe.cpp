#include "safe.hpp"

#include <cpl_conv.h>
#include <cpl_minixml.h>
#include <cpl_string.h>
#include <cpl_vsi.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

long long daysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097LL + int(doe) - 719468;
}

std::vector<double> parseDoubles(const char* text) {
    std::vector<double> v;
    if (!text) return v;
    const char* p = text;
    while (*p) {
        while (*p && std::isspace((unsigned char)*p)) ++p;
        if (!*p) break;
        char* end = nullptr;
        const double d = std::strtod(p, &end);
        if (end == p) break;
        v.push_back(d);
        p = end;
    }
    return v;
}

void expandRow(const std::vector<double>& x, const std::vector<double>& y,
               int n, float* out) {
    if (x.empty() || x.size() != y.size()) {
        for (int i = 0; i < n; ++i) out[i] = 1.0f;
        return;
    }
    size_t k = 0;
    for (int i = 0; i < n; ++i) {
        const double xi = double(i);
        while (k + 2 < x.size() && x[k + 1] < xi) ++k;
        const double x0 = x[k], x1 = x[std::min(k + 1, x.size() - 1)];
        const double y0 = y[k], y1 = y[std::min(k + 1, y.size() - 1)];
        const double w = (x1 > x0) ? (xi - x0) / (x1 - x0) : 0.0;
        out[i] = float(y0 + (y1 - y0) * w);
    }
}

std::string lower(std::string s) {
    for (char& c : s) c = char(std::tolower((unsigned char)c));
    return s;
}

std::vector<std::string> listDir(const std::string& dir) {
    std::vector<std::string> out;
    char** files = VSIReadDir(dir.c_str());
    if (!files) return out;
    for (int i = 0; files[i]; ++i) {
        const std::string n = files[i];
        if (n != "." && n != "..") out.push_back(n);
    }
    CSLDestroy(files);
    return out;
}

std::string findFile(const std::string& dir, const std::vector<std::string>& needles) {
    for (const std::string& n : listDir(dir)) {
        const std::string ln = lower(n);
        bool all = true;
        for (const std::string& k : needles)
            if (ln.find(lower(k)) == std::string::npos) { all = false; break; }
        if (all) return dir + "/" + n;
    }
    return {};
}

const char* xval(CPLXMLNode* root, const char* path, const char* dflt = nullptr) {
    return CPLGetXMLValue(root, path, dflt);
}

}

double s1TimeToSeconds(const std::string& iso) {
    int Y = 0, M = 0, D = 0, h = 0, m = 0;
    double s = 0.0;
    if (std::sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%lf", &Y, &M, &D, &h, &m, &s) != 6)
        return 0.0;
    const long long days = daysFromCivil(Y, unsigned(M), unsigned(D))
                         - daysFromCivil(2000, 1, 1);
    return double(days) * 86400.0 + h * 3600.0 + m * 60.0 + s;
}

bool SafeProduct::readAnnotation(const std::string& path) {
    CPLXMLNode* root = CPLParseXMLFile(path.c_str());
    if (!root) { std::fprintf(stderr, "[safe] cannot parse %s\n", path.c_str()); return false; }

    CPLXMLNode* p = CPLGetXMLNode(root, "=product");
    if (!p) p = root;

    ns = std::atoi(xval(p, "imageAnnotation.imageInformation.numberOfSamples", "0"));
    nl = std::atoi(xval(p, "imageAnnotation.imageInformation.numberOfLines", "0"));
    startISO = xval(p, "imageAnnotation.imageInformation.productFirstLineUtcTime", "");
    stopISO  = xval(p, "imageAnnotation.imageInformation.productLastLineUtcTime", "");
    t0   = s1TimeToSeconds(startISO);
    t1   = s1TimeToSeconds(stopISO);
    dtAz = CPLAtof(xval(p, "imageAnnotation.imageInformation.azimuthTimeInterval", "0"));
    rgSpacing = CPLAtof(xval(p, "imageAnnotation.imageInformation.rangePixelSpacing", "0"));
    const double freq = CPLAtof(xval(p, "generalAnnotation.productInformation.radarFrequency", "0"));
    wavelength = freq > 0 ? 299792458.0 / freq : 0.0;

    const char* proj = xval(p, "generalAnnotation.productInformation.projection", "");
    const char* ptype = xval(p, "adsHeader.productType", "");
    if (!proj || !*proj) {
        std::fprintf(stderr, "[safe] WARNING no projection field in the annotation\n");
    } else if (std::strcmp(proj, "Ground Range") != 0) {
        std::fprintf(stderr, "[safe] ERROR projection is '%s' (productType '%s'), not "
                             "'Ground Range'. This reader handles GRD only; an SLC needs "
                             "the slant-range path and would be geocoded wrongly.\n",
                     proj, ptype ? ptype : "?");
        CPLDestroyXMLNode(root);
        return false;
    }

    CPLXMLNode* ol = CPLGetXMLNode(p, "generalAnnotation.orbitList");
    for (CPLXMLNode* n = ol ? ol->psChild : nullptr; n; n = n->psNext) {
        if (n->eType != CXT_Element || !EQUAL(n->pszValue, "orbit")) continue;
        OrbitVec o{};
        o.t = s1TimeToSeconds(xval(n, "time", ""));
        o.p[0] = CPLAtof(xval(n, "position.x", "0"));
        o.p[1] = CPLAtof(xval(n, "position.y", "0"));
        o.p[2] = CPLAtof(xval(n, "position.z", "0"));
        o.v[0] = CPLAtof(xval(n, "velocity.x", "0"));
        o.v[1] = CPLAtof(xval(n, "velocity.y", "0"));
        o.v[2] = CPLAtof(xval(n, "velocity.z", "0"));
        orbit.push_back(o);
    }
    std::sort(orbit.begin(), orbit.end(),
              [](const OrbitVec& a, const OrbitVec& b) { return a.t < b.t; });

    CPLXMLNode* cl = CPLGetXMLNode(p, "coordinateConversion.coordinateConversionList");
    for (CPLXMLNode* n = cl ? cl->psChild : nullptr; n; n = n->psNext) {
        if (n->eType != CXT_Element || !EQUAL(n->pszValue, "coordinateConversion")) continue;
        SrgrSet s{};
        s.t   = s1TimeToSeconds(xval(n, "azimuthTime", ""));
        s.sr0 = CPLAtof(xval(n, "sr0", "0"));
        s.gr0 = CPLAtof(xval(n, "gr0", "0"));
        const std::vector<double> c = parseDoubles(xval(n, "srgrCoefficients", ""));
        s.nc = int(std::min<size_t>(c.size(), 12));
        for (int i = 0; i < s.nc; ++i) s.c[i] = c[size_t(i)];
        srgr.push_back(s);
    }
    std::sort(srgr.begin(), srgr.end(),
              [](const SrgrSet& a, const SrgrSet& b) { return a.t < b.t; });

    CPLXMLNode* gl = CPLGetXMLNode(p, "geolocationGrid.geolocationGridPointList");
    for (CPLXMLNode* n = gl ? gl->psChild : nullptr; n; n = n->psNext) {
        if (n->eType != CXT_Element || !EQUAL(n->pszValue, "geolocationGridPoint")) continue;
        SafeGcp g{};
        g.line   = CPLAtof(xval(n, "line", "0"));
        g.pixel  = CPLAtof(xval(n, "pixel", "0"));
        g.lat    = CPLAtof(xval(n, "latitude", "0"));
        g.lon    = CPLAtof(xval(n, "longitude", "0"));
        g.height = CPLAtof(xval(n, "height", "0"));
        gcps.push_back(g);
    }

    CPLDestroyXMLNode(root);

    if (ns <= 0 || nl <= 0 || orbit.size() < 2 || srgr.empty() || gcps.empty()) {
        std::fprintf(stderr, "[safe] annotation incomplete: ns=%d nl=%d orbit=%zu "
                             "srgr=%zu gcps=%zu\n", ns, nl, orbit.size(), srgr.size(),
                     gcps.size());
        return false;
    }
    return true;
}

bool SafeProduct::readCalibration(const std::string& path) {
    CPLXMLNode* root = CPLParseXMLFile(path.c_str());
    if (!root) { std::fprintf(stderr, "[safe] cannot parse %s\n", path.c_str()); return false; }

    CPLXMLNode* list = CPLGetXMLNode(root, "=calibration.calibrationVectorList");
    if (!list) list = CPLGetXMLNode(root, "calibration.calibrationVectorList");
    std::vector<std::pair<int, std::vector<float>>> rows;

    for (CPLXMLNode* n = list ? list->psChild : nullptr; n; n = n->psNext) {
        if (n->eType != CXT_Element || !EQUAL(n->pszValue, "calibrationVector")) continue;
        const int line = std::atoi(xval(n, "line", "0"));
        const std::vector<double> px = parseDoubles(xval(n, "pixel", ""));
        const std::vector<double> sg = parseDoubles(xval(n, "sigmaNought", ""));
        std::vector<float> dense(static_cast<size_t>(ns), 0.0f);
        expandRow(px, sg, ns, dense.data());
        rows.emplace_back(line, std::move(dense));
    }
    CPLDestroyXMLNode(root);

    if (rows.empty()) { std::fprintf(stderr, "[safe] no calibration vectors\n"); return false; }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });

    calLines.reserve(rows.size());
    calRows.resize(rows.size() * size_t(ns));
    for (size_t i = 0; i < rows.size(); ++i) {
        calLines.push_back(rows[i].first);
        std::copy(rows[i].second.begin(), rows[i].second.end(),
                  calRows.begin() + long(i * size_t(ns)));
    }
    return true;
}

bool SafeProduct::readNoise(const std::string& path) {
    CPLXMLNode* root = CPLParseXMLFile(path.c_str());
    if (!root) { std::fprintf(stderr, "[safe] cannot parse %s\n", path.c_str()); return false; }

    CPLXMLNode* rl = CPLGetXMLNode(root, "=noise.noiseRangeVectorList");
    if (!rl) rl = CPLGetXMLNode(root, "noise.noiseRangeVectorList");

    std::vector<std::pair<int, std::vector<float>>> rows;
    for (CPLXMLNode* n = rl ? rl->psChild : nullptr; n; n = n->psNext) {
        if (n->eType != CXT_Element || !EQUAL(n->pszValue, "noiseRangeVector")) continue;
        const int line = std::atoi(xval(n, "line", "0"));
        const std::vector<double> px = parseDoubles(xval(n, "pixel", ""));
        const std::vector<double> lu = parseDoubles(xval(n, "noiseRangeLut", ""));
        std::vector<float> dense(static_cast<size_t>(ns), 0.0f);
        expandRow(px, lu, ns, dense.data());
        rows.emplace_back(line, std::move(dense));
    }

    CPLXMLNode* al = CPLGetXMLNode(root, "=noise.noiseAzimuthVectorList");
    if (!al) al = CPLGetXMLNode(root, "noise.noiseAzimuthVectorList");
    for (CPLXMLNode* n = al ? al->psChild : nullptr; n; n = n->psNext) {
        if (n->eType != CXT_Element || !EQUAL(n->pszValue, "noiseAzimuthVector")) continue;
        const std::vector<double> ln = parseDoubles(xval(n, "line", ""));
        const std::vector<double> lu = parseDoubles(xval(n, "noiseAzimuthLut", ""));
        if (ln.empty() || lu.empty()) continue;
        azFirstSample.push_back(std::atoi(xval(n, "firstRangeSample", "0")));
        azLastSample.push_back(std::atoi(xval(n, "lastRangeSample", "0")));
        const size_t off = azNoise.size();
        azNoise.resize(off + size_t(nl));
        expandRow(ln, lu, nl, azNoise.data() + off);
    }
    CPLDestroyXMLNode(root);

    if (rows.empty()) {
        std::fprintf(stderr, "[safe] no noise vectors; thermal noise will not be removed\n");
        return true;
    }
    std::sort(rows.begin(), rows.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    noiseLines.reserve(rows.size());
    noiseRows.resize(rows.size() * size_t(ns));
    for (size_t i = 0; i < rows.size(); ++i) {
        noiseLines.push_back(rows[i].first);
        std::copy(rows[i].second.begin(), rows[i].second.end(),
                  noiseRows.begin() + long(i * size_t(ns)));
    }
    return true;
}

bool SafeProduct::open(const std::string& path, const std::string& pol) {
    polarisation = pol;

    std::string root = path;
    while (!root.empty() && root.back() == '/') root.pop_back();
    basename = root.substr(root.find_last_of('/') + 1);

    if (lower(root).size() > 4 && lower(root).compare(root.size() - 4, 4, ".zip") == 0) {
        std::string vz = "/vsizip/" + root;
        const std::vector<std::string> top = listDir(vz);
        if (top.size() == 1 && lower(top[0]).find(".safe") != std::string::npos) {
            vz += "/" + top[0];
            basename = top[0];
        }
        root = vz;
    }
    if (basename.size() > 5 && lower(basename).compare(basename.size() - 5, 5, ".safe") == 0)
        basename = basename.substr(0, basename.size() - 5);

    const std::string annDir = root + "/annotation";
    const std::string calDir = annDir + "/calibration";
    const std::string measDir = root + "/measurement";

    const std::string p = lower(pol);
    const std::string ann  = findFile(annDir, {"-" + p + "-", ".xml"});
    const std::string cal  = findFile(calDir, {"calibration-", "-" + p + "-"});
    const std::string noi  = findFile(calDir, {"noise-", "-" + p + "-"});
    const std::string meas = findFile(measDir, {"-" + p + "-"});

    if (ann.empty() || meas.empty()) {
        std::fprintf(stderr,
            "[safe] could not find %s annotation/measurement under %s\n"
            "       (looked in %s and %s)\n",
            pol.c_str(), root.c_str(), annDir.c_str(), measDir.c_str());
        return false;
    }

    if (!readAnnotation(ann)) return false;
    if (!cal.empty() && !readCalibration(cal)) return false;
    if (calLines.empty()) {
        std::fprintf(stderr, "[safe] no calibration LUT for %s; cannot produce sigma0\n",
                     pol.c_str());
        return false;
    }
    if (!noi.empty() && !readNoise(noi)) return false;

    measurement = meas;

    std::fprintf(stderr,
        "[safe] %s  %s  %d x %d  %.1f m  %d orbit vec, %d srgr sets, "
        "%d cal rows, %d noise rows, %d swaths\n",
        basename.c_str(), pol.c_str(), ns, nl, rgSpacing,
        int(orbit.size()), int(srgr.size()), nCal(), nNoise(), nSwath());
    return true;
}
