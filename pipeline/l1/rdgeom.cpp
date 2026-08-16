#include "rdgeom.hpp"

#include <algorithm>
#include <cmath>

namespace {
constexpr double A_WGS84 = 6378137.0;
constexpr double F_WGS84 = 1.0 / 298.257223563;
constexpr double E2_WGS84 = F_WGS84 * (2.0 - F_WGS84);
}

void geodeticToEcef(double latDeg, double lonDeg, double h, Vec3& p, Vec3& normal) {
    const double lat = latDeg * M_PI / 180.0, lon = lonDeg * M_PI / 180.0;
    const double sl = std::sin(lat), cl = std::cos(lat);
    const double so = std::sin(lon), co = std::cos(lon);
    const double N = A_WGS84 / std::sqrt(1.0 - E2_WGS84 * sl * sl);
    normal = {cl * co, cl * so, sl};
    p = {(N + h) * cl * co, (N + h) * cl * so, (N * (1.0 - E2_WGS84) + h) * sl};
}

void orbitLagrange(const std::vector<OrbitVec>& sv, double t, Vec3& p, Vec3& v) {
    const int n = int(sv.size());
    if (n == 0) { p = {}; v = {}; return; }
    const int k = std::min(8, n);
    int lo = 0;
    while (lo < n && sv[size_t(lo)].t < t) ++lo;
    const int i0 = std::max(0, std::min(lo - k / 2, n - k));
    p = {}; v = {};
    for (int i = 0; i < k; ++i) {
        double w = 1.0;
        const double ti = sv[size_t(i0 + i)].t;
        for (int j = 0; j < k; ++j) {
            if (i == j) continue;
            const double tj = sv[size_t(i0 + j)].t;
            w *= (t - tj) / (ti - tj);
        }
        const OrbitVec& s = sv[size_t(i0 + i)];
        p.x += w * s.p[0]; p.y += w * s.p[1]; p.z += w * s.p[2];
        v.x += w * s.v[0]; v.y += w * s.v[1]; v.z += w * s.v[2];
    }
}

void OrbitTable::build(const SafeProduct& sp, Vec3 org,
                       double tStart, double tEnd, double step) {
    origin = org;
    t0 = tStart;
    dt = step;
    n = int((tEnd - tStart) / step) + 2;

    p.resize(size_t(n)); v.resize(size_t(n)); a.resize(size_t(n));
    for (int i = 0; i < n; ++i) {
        Vec3 pp, vv;
        orbitLagrange(sp.orbit, sp.t0 + t0 + i * dt, pp, vv);
        p[size_t(i)] = pp - origin;
        v[size_t(i)] = vv;
    }
    for (int i = 0; i < n; ++i) {
        const int lo = std::max(0, i - 1), hi = std::min(n - 1, i + 1);
        const double h = (hi - lo) * dt;
        a[size_t(i)] = {(v[size_t(hi)].x - v[size_t(lo)].x) / h,
                        (v[size_t(hi)].y - v[size_t(lo)].y) / h,
                        (v[size_t(hi)].z - v[size_t(lo)].z) / h};
    }
}

void OrbitTable::sample(double t, Vec3& P, Vec3& V, Vec3& A) const {
    double fi = (t - t0) / dt;
    fi = std::max(0.0, std::min(fi, double(n - 2)));
    const int i = int(fi);
    const double w = fi - i;
    const Vec3& p0 = p[size_t(i)]; const Vec3& p1 = p[size_t(i + 1)];
    const Vec3& v0 = v[size_t(i)]; const Vec3& v1 = v[size_t(i + 1)];
    const Vec3& a0 = a[size_t(i)]; const Vec3& a1 = a[size_t(i + 1)];
    P = {p0.x + w * (p1.x - p0.x), p0.y + w * (p1.y - p0.y), p0.z + w * (p1.z - p0.z)};
    V = {v0.x + w * (v1.x - v0.x), v0.y + w * (v1.y - v0.y), v0.z + w * (v1.z - v0.z)};
    A = {a0.x + w * (a1.x - a0.x), a0.y + w * (a1.y - a0.y), a0.z + w * (a1.z - a0.z)};
}

double zeroDoppler(const OrbitTable& tab, Vec3 T, double seed, bool haveSeed) {
    Vec3 P, V, A;
    double t = seed;
    if (!haveSeed) {
        double lo = tab.t0, hi = tab.t0 + (tab.n - 1) * tab.dt;
        tab.sample(lo, P, V, A);
        double flo = dot(P - T, V);
        for (int i = 0; i < 40; ++i) {
            const double mid = 0.5 * (lo + hi);
            tab.sample(mid, P, V, A);
            const double fm = dot(P - T, V);
            if ((fm > 0) == (flo > 0)) { lo = mid; flo = fm; } else { hi = mid; }
        }
        t = 0.5 * (lo + hi);
    }
    for (int i = 0; i < 5; ++i) {
        tab.sample(t, P, V, A);
        const Vec3 d = P - T;
        const double f = dot(d, V);
        const double fp = dot(V, V) + dot(d, A);
        if (fp == 0.0) break;
        const double stepT = f / fp;
        t -= stepT;
        if (std::fabs(stepT) < 1e-9) break;
    }
    return t;
}

double srgrGroundRange(const std::vector<SrgrSet>& sets, double t0Abs,
                       double tRel, double sr) {
    if (sets.empty()) return sr;
    const int n = int(sets.size());
    int i = 0;
    while (i + 2 < n && (sets[size_t(i + 1)].t - t0Abs) < tRel) ++i;
    const int j = std::min(i + 1, n - 1);
    const double ta = sets[size_t(i)].t - t0Abs, tb = sets[size_t(j)].t - t0Abs;
    double w = (tb > ta) ? (tRel - ta) / (tb - ta) : 0.0;
    w = std::max(0.0, std::min(1.0, w));

    const SrgrSet& s0 = sets[size_t(i)];
    const SrgrSet& s1 = sets[size_t(j)];
    const double sr0 = (1 - w) * s0.sr0 + w * s1.sr0;
    const double gr0 = (1 - w) * s0.gr0 + w * s1.gr0;
    const double d = sr - sr0;

    double gr = 0.0, pw = 1.0;
    const int nc = std::max(s0.nc, s1.nc);
    for (int k = 0; k < nc; ++k) {
        const double ck = (1 - w) * (k < s0.nc ? s0.c[k] : 0.0)
                        + w * (k < s1.nc ? s1.c[k] : 0.0);
        gr += ck * pw;
        pw *= d;
    }
    return gr - gr0;
}
