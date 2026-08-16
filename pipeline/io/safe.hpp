#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct OrbitVec {
    double t;
    double p[3], v[3];
};

struct SrgrSet {
    double t;
    double sr0, gr0;
    double c[12];
    int    nc = 0;
};

struct SafeGcp {
    double line, pixel, lat, lon, height;
};

class SafeProduct {
public:
    bool open(const std::string& path, const std::string& pol);

    int ns = 0, nl = 0;
    std::string measurement;

    double t0 = 0.0;
    double t1 = 0.0;
    double dtAz = 0.0;
    double rgSpacing = 0.0;
    double wavelength = 0.0;

    std::vector<OrbitVec> orbit;
    std::vector<SrgrSet>  srgr;
    std::vector<SafeGcp>  gcps;

    std::string basename, startISO, stopISO, polarisation;

    std::vector<float> calRows;
    std::vector<int>   calLines;
    std::vector<float> noiseRows;
    std::vector<int>   noiseLines;

    std::vector<float> azNoise;
    std::vector<int>   azFirstSample, azLastSample;

    int nCal()   const { return int(calLines.size()); }
    int nNoise() const { return int(noiseLines.size()); }
    int nSwath() const { return int(azFirstSample.size()); }

private:
    bool readAnnotation(const std::string& path);
    bool readCalibration(const std::string& path);
    bool readNoise(const std::string& path);
};

double s1TimeToSeconds(const std::string& iso);
