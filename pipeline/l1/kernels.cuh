#pragma once
#include <cuda_runtime.h>
#include <cuda_fp16.h>
#include <cstdint>

void launch_calibrate(const uint16_t* dn, int ns, int nl,
                      const float* calRows, const int* calIdx, const float* calW,
                      const float* noiseRows, const int* noiIdx, const float* noiW,
                      const float* azNoise, const int* swathOf, int nl_az,
                      __half* sigma0, int y0, int nRows,
                      float noiseScale, cudaStream_t s);

struct RdParams {
    int   W, H;
    int   cw, ch, cstep;
    int   ns, nl;
    float dtAz, rgSpacing;
    float tabT0, tabDt;
    int   tabN;
    int   demW, demH;
    float demLon0, demLat0, demDLon, demDLat;
    float demNoData;
    const unsigned char* swath;
};

struct SrgrDev {
    double t, sr0, gr0;
    double c[12];
    int    nc, _pad;
};

struct SrgrLut {
    const float* tab;
    const float* sr0;
    const float* gr0;
    const float* t0;
    const float* invDt;
    int   nSet, n;
    float ddMin, invStep;
};

void launch_swath_mask(const RdParams& prm,
                       const float3* cT0, const float3* cU, const float* cTseed,
                       const float2* cLonLat, const float* dem,
                       const float3* orbP, const float3* orbV, const float3* orbA,
                       const SrgrDev* srgr, const int* srgrIdx,
                       unsigned char* mask, cudaStream_t s);

struct RdQuant {
    unsigned char* gray     = nullptr;
    unsigned char* dataMask = nullptr;
    unsigned int*  maxGrid  = nullptr;
    unsigned int*  sumGrid  = nullptr;
    int   cw = 0;
    int   decim = 1, shift = -1;
    float lo = 0.0f, invSpan = 1.0f;
};

void launch_rd_geocode(const RdParams& prm,
                       const float3* cT0, const float3* cU, const float* cTseed,
                       const float2* cLonLat,
                       const float* dem,
                       const float3* orbP, const float3* orbV, const float3* orbA,
                       const SrgrDev* srgr, const int* srgrIdx,
                       const __half* sigma0, float* out, const RdQuant& q,
                       const SrgrLut& lut, cudaStream_t s);
