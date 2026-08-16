#pragma once
#include <vector>

#include "../io/safe.hpp"

struct Vec3 {
    double x = 0, y = 0, z = 0;
};

inline Vec3 operator-(Vec3 a, Vec3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
inline double dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

void geodeticToEcef(double latDeg, double lonDeg, double h, Vec3& p, Vec3& normal);

void orbitLagrange(const std::vector<OrbitVec>& sv, double t, Vec3& p, Vec3& v);

struct OrbitTable {
    double t0 = 0.0;
    double dt = 1e-3;
    int    n = 0;
    std::vector<Vec3> p, v, a;
    Vec3 origin{};

    void build(const SafeProduct& sp, Vec3 org, double tStart, double tEnd, double step);
    void sample(double t, Vec3& P, Vec3& V, Vec3& A) const;
};

double zeroDoppler(const OrbitTable& tab, Vec3 T, double seed, bool haveSeed);

double srgrGroundRange(const std::vector<SrgrSet>& sets, double t0Abs,
                       double tRel, double sr);
