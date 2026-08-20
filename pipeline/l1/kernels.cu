#include "kernels.cuh"
#include "common/quantise.cuh"

__global__ void k_calibrate(const uint16_t* __restrict__ dn, int ns, int nl,
                            const float* __restrict__ calRows,
                            const int* __restrict__ calIdx,
                            const float* __restrict__ calW,
                            const float* __restrict__ noiseRows,
                            const int* __restrict__ noiIdx,
                            const float* __restrict__ noiW,
                            const float* __restrict__ azNoise,
                            const int* __restrict__ swathOf,
                            __half* __restrict__ sigma0, int y0, int nRows,
                            float noiseScale) {
    for (int y = y0 + blockIdx.y; y < y0 + nRows; y += gridDim.y) {
        const size_t row = size_t(y) * ns;
        const int   ci = calIdx[y];  const float cwgt = calW[y];
        const float* c0 = calRows + size_t(ci) * ns;
        const float* c1 = calRows + size_t(ci + 1) * ns;

        const bool haveNoise = (noiIdx != nullptr);
        const int   ni = haveNoise ? noiIdx[y] : 0;
        const float nwgt = haveNoise ? noiW[y] : 0.0f;
        const float* n0 = haveNoise ? noiseRows + size_t(ni) * ns : nullptr;
        const float* n1 = haveNoise ? noiseRows + size_t(ni + 1) * ns : nullptr;

        for (int x = blockIdx.x * blockDim.x + threadIdx.x; x < ns;
             x += gridDim.x * blockDim.x) {
            const float cal = fmaf(cwgt, c1[x] - c0[x], c0[x]);

            float noise = 0.0f;
            if (haveNoise) {
                noise = fmaf(nwgt, n1[x] - n0[x], n0[x]);
                const int sw = swathOf[x];
                if (sw >= 0) noise *= azNoise[size_t(sw) * nl + y];
            }

            const float d = float(dn[row + x]);
            const float p = fmaf(d, d, -noise * noiseScale);
            const float sig = (p > 0.0f && cal > 0.0f) ? p / (cal * cal) : 0.0f;
            sigma0[row + x] = __float2half(fminf(sig, 60000.0f));
        }
    }
}

void launch_calibrate(const uint16_t* dn, int ns, int nl,
                      const float* calRows, const int* calIdx, const float* calW,
                      const float* noiseRows, const int* noiIdx, const float* noiW,
                      const float* azNoise, const int* swathOf, int nl_az,
                      __half* sigma0, int y0, int nRows,
                      float noiseScale, cudaStream_t s) {
    (void)nl_az;
    if (nRows <= 0) return;

    dim3 grid((ns + 255) / 256, nRows);
    k_calibrate<<<grid, 256, 0, s>>>(dn, ns, nl, calRows, calIdx, calW,
                                     noiseRows, noiIdx, noiW, azNoise, swathOf,
                                     sigma0, y0, nRows, noiseScale);
}

__device__ __forceinline__ float3 f3(float a, float b, float c) {
    float3 r; r.x = a; r.y = b; r.z = c; return r;
}
__device__ __forceinline__ float3 operator+(float3 a, float3 b) { return f3(a.x+b.x, a.y+b.y, a.z+b.z); }
__device__ __forceinline__ float3 operator-(float3 a, float3 b) { return f3(a.x-b.x, a.y-b.y, a.z-b.z); }
__device__ __forceinline__ float3 operator*(float s, float3 a)  { return f3(s*a.x, s*a.y, s*a.z); }
__device__ __forceinline__ float  dot3(float3 a, float3 b) { return a.x*b.x + a.y*b.y + a.z*b.z; }

__device__ __forceinline__ float3 clat3(const float3* g, int cw, int gx, int gy,
                                        float fx, float fy) {
    const float3 a = g[size_t(gy) * cw + gx],       b = g[size_t(gy) * cw + gx + 1];
    const float3 c = g[size_t(gy + 1) * cw + gx],   d = g[size_t(gy + 1) * cw + gx + 1];
    return (1.0f - fy) * ((1.0f - fx) * a + fx * b) + fy * ((1.0f - fx) * c + fx * d);
}

__device__ __forceinline__ float clat1(const float* g, int cw, int gx, int gy,
                                       float fx, float fy) {
    const float a = g[size_t(gy) * cw + gx],     b = g[size_t(gy) * cw + gx + 1];
    const float c = g[size_t(gy + 1) * cw + gx], d = g[size_t(gy + 1) * cw + gx + 1];
    return (1.0f - fy) * (a + fx * (b - a)) + fy * (c + fx * (d - c));
}

__device__ __forceinline__ float2 clat2(const float2* g, int cw, int gx, int gy,
                                        float fx, float fy) {
    const float2 a = g[size_t(gy) * cw + gx],     b = g[size_t(gy) * cw + gx + 1];
    const float2 c = g[size_t(gy + 1) * cw + gx], d = g[size_t(gy + 1) * cw + gx + 1];
    float2 r;
    r.x = (1.0f - fy) * (a.x + fx * (b.x - a.x)) + fy * (c.x + fx * (d.x - c.x));
    r.y = (1.0f - fy) * (a.y + fx * (b.y - a.y)) + fy * (c.y + fx * (d.y - c.y));
    return r;
}

#ifndef RD_TILE
#define RD_TILE 32
#endif

#ifndef RD_NEWTON_ITERS
#define RD_NEWTON_ITERS 3
#endif

#ifndef RD_FP32_SRGR
#define RD_FP32_SRGR 0
#endif

#ifndef RD_PROBE_NOPOLY
#define RD_PROBE_NOPOLY 0
#endif

#ifndef RD_SRGR_LUT
#define RD_SRGR_LUT 1
#endif

__device__ __forceinline__ void rd_solve(const RdParams& P, float3 T, float t,
                                         const float3* __restrict__ orbP,
                                         const float3* __restrict__ orbV,
                                         const float3* __restrict__ orbA,
                                         const SrgrDev* __restrict__ srgr,
                                         const int* __restrict__ srgrIdx,
                                         float& srcX, float& srcY) {
    float3 d3 = f3(0.f, 0.f, 0.f);
    for (int it = 0; it < RD_NEWTON_ITERS; ++it) {
        float fi = (t - P.tabT0) / P.tabDt;
        fi = fminf(fmaxf(fi, 0.0f), float(P.tabN - 2));
        const int i = int(fi);
        const float w = fi - i;
        const float3 p = orbP[i] + w * (orbP[i + 1] - orbP[i]);
        const float3 v = orbV[i] + w * (orbV[i + 1] - orbV[i]);
        const float3 a = orbA[i] + w * (orbA[i + 1] - orbA[i]);
        d3 = p - T;
        const float f  = dot3(d3, v);
        const float fp = dot3(v, v) + dot3(d3, a);
        if (fp != 0.0f) t -= f / fp;
    }
    srcY = t / P.dtAz;
    const double sr = sqrt(double(dot3(d3, d3)));
    int si = 0;
    {
        float fi = (t - P.tabT0) / P.tabDt;
        fi = fminf(fmaxf(fi, 0.0f), float(P.tabN - 1));
        si = srgrIdx[int(fi)];
    }
    const SrgrDev& s0 = srgr[si];
    const SrgrDev& s1 = srgr[si + 1];
    const double dtw = (s1.t > s0.t) ? (double(t) - s0.t) / (s1.t - s0.t) : 0.0;
    const double w1 = dtw < 0.0 ? 0.0 : (dtw > 1.0 ? 1.0 : dtw), w0 = 1.0 - w1;
    const double sr0 = w0 * s0.sr0 + w1 * s1.sr0;
    const double gr0 = w0 * s0.gr0 + w1 * s1.gr0;
    const double dd = sr - sr0;
    double gr = 0.0, pw = 1.0;
    const int nc = max(s0.nc, s1.nc);
    for (int k = 0; k < nc; ++k) {
        const double ck = w0 * (k < s0.nc ? s0.c[k] : 0.0)
                        + w1 * (k < s1.nc ? s1.c[k] : 0.0);
        gr += ck * pw;
        pw *= dd;
    }
    srcX = float((gr - gr0) / P.rgSpacing);
}

__global__ void k_swath_mask(RdParams P,
                             const float3* __restrict__ cT0,
                             const float3* __restrict__ cU,
                             const float* __restrict__ cTseed,
                             const float2* __restrict__ cLonLat,
                             const float* __restrict__ dem,
                             const float3* __restrict__ orbP,
                             const float3* __restrict__ orbV,
                             const float3* __restrict__ orbA,
                             const SrgrDev* __restrict__ srgr,
                             const int* __restrict__ srgrIdx,
                             unsigned char* __restrict__ mask) {
    const int total = P.cw * P.ch;
    for (int k = blockIdx.x * blockDim.x + threadIdx.x; k < total;
         k += gridDim.x * blockDim.x) {
        float h = 0.0f;
        if (dem) {
            const float2 ll = cLonLat[k];
            const float dx = (ll.x - P.demLon0) / P.demDLon;
            const float dy = (ll.y - P.demLat0) / P.demDLat;
            const int ix = int(floorf(dx)), iy = int(floorf(dy));
            if (ix >= 0 && iy >= 0 && ix < P.demW - 1 && iy < P.demH - 1) {
                h = dem[size_t(iy) * P.demW + ix];
                if (h == P.demNoData) h = 0.0f;
            }
        }
        const float3 T = cT0[k] + h * cU[k];
        float sx = 0.0f, sy = 0.0f;
        rd_solve(P, T, cTseed[k], orbP, orbV, orbA, srgr, srgrIdx, sx, sy);
        const float M = float(4 * P.cstep);
        mask[k] = (sx > -M && sx < float(P.ns) + M &&
                   sy > -M && sy < float(P.nl) + M) ? 1 : 0;
    }
}

void launch_swath_mask(const RdParams& prm,
                       const float3* cT0, const float3* cU, const float* cTseed,
                       const float2* cLonLat, const float* dem,
                       const float3* orbP, const float3* orbV, const float3* orbA,
                       const SrgrDev* srgr, const int* srgrIdx,
                       unsigned char* mask, cudaStream_t s) {
    k_swath_mask<<<256, 256, 0, s>>>(prm, cT0, cU, cTseed, cLonLat, dem,
                                     orbP, orbV, orbA, srgr, srgrIdx, mask);
}

__device__ __forceinline__ float srgr_lut(const SrgrLut& L, int set, float dd) {
    const float u = fminf(fmaxf((dd - L.ddMin) * L.invStep, 0.0f), float(L.n - 2));
    const int   j = int(u);
    const float f = u - j;
    const float* r = L.tab + size_t(set) * L.n + j;
    return fmaf(f, r[1] - r[0], r[0]);
}

__device__ __forceinline__ void rd_emit(float val, size_t idx, size_t c,
                                        float* __restrict__ outF,
                                        const RdQuant& q) {
    if (outF) outF[idx] = val;
    if (!q.gray) return;

    const unsigned char g = quant::sigma_to_u8(val, q.lo, q.invSpan);
    q.gray[idx] = g;

    if (val > 0.0f) {
        unsigned char* p = q.dataMask + c;
        if (!*p) *p = 1;
    }

    unsigned int sum = g, mx = g;
    bool leader = true;
    const unsigned act = __activemask();
    if (act == 0xffffffffu && q.shift >= 0 && q.decim > 1 && q.decim <= 32) {
        for (int off = 1; off < q.decim; off <<= 1) {
            sum += __shfl_down_sync(act, sum, off, q.decim);
            mx   = max(mx, __shfl_down_sync(act, mx, off, q.decim));
        }
        leader = (threadIdx.x & (q.decim - 1)) == 0;
    }
    if (leader && mx) {
        unsigned int* xp = q.maxGrid + c;
        if (mx > *xp) atomicMax(xp, mx);
        atomicAdd(q.sumGrid + c, sum);
    }
}

__global__ void k_rd_geocode(RdParams P,
                             const float3* __restrict__ cT0,
                             const float3* __restrict__ cU,
                             const float* __restrict__ cTseed,
                             const float2* __restrict__ cLonLat,
                             const float* __restrict__ dem,
                             const float3* __restrict__ orbP,
                             const float3* __restrict__ orbV,
                             const float3* __restrict__ orbA,
                             const SrgrDev* __restrict__ srgr,
                             const int* __restrict__ srgrIdx,
                             const __half* __restrict__ sigma0,
                             float* __restrict__ out,
                             RdQuant q,
                             SrgrLut L) {
    const float invStep = 1.0f / float(P.cstep);

    const int x = blockIdx.x * RD_TILE + threadIdx.x;
    if (x >= P.W) return;

    const float gxf = x * invStep;
    int gx = int(gxf); gx = min(gx, P.cw - 2);
    const float fx = gxf - gx;
    const int cx = (q.shift >= 0) ? (x >> q.shift) : (x / q.decim);

    const int yEnd = min(blockIdx.y * RD_TILE + RD_TILE, P.H);
    for (int y = blockIdx.y * RD_TILE + threadIdx.y; y < yEnd; y += blockDim.y) {
        const float gyf = y * invStep;
        int gy = int(gyf); gy = min(gy, P.ch - 2);
        const float fy = gyf - gy;

        const size_t cell =
            q.gray ? size_t((q.shift >= 0) ? (y >> q.shift) : (y / q.decim)) * q.cw + cx
                   : 0;

        {

            if (P.swath) {
                const int i0 = gy * P.cw + gx;
                if (!P.swath[i0] && !P.swath[i0 + 1]
                    && !P.swath[i0 + P.cw] && !P.swath[i0 + P.cw + 1]) {
                    rd_emit(0.0f, size_t(y) * P.W + x, cell, out, q);
                    continue;
                }
            }

            float h = 0.0f;
            if (dem) {
                const float2 ll = clat2(cLonLat, P.cw, gx, gy, fx, fy);
                const float dx = (ll.x - P.demLon0) / P.demDLon;
                const float dy = (ll.y - P.demLat0) / P.demDLat;
                const int ix = int(floorf(dx)), iy = int(floorf(dy));
                if (ix >= 0 && iy >= 0 && ix < P.demW - 1 && iy < P.demH - 1) {
                    const float tx = dx - ix, ty = dy - iy;
                    const float a = dem[size_t(iy) * P.demW + ix];
                    const float b = dem[size_t(iy) * P.demW + ix + 1];
                    const float c = dem[size_t(iy + 1) * P.demW + ix];
                    const float d = dem[size_t(iy + 1) * P.demW + ix + 1];
                    h = (1.0f - ty) * (a + tx * (b - a)) + ty * (c + tx * (d - c));
                    if (h == P.demNoData) h = 0.0f;
                }
            }

            const float3 T = clat3(cT0, P.cw, gx, gy, fx, fy)
                           + h * clat3(cU, P.cw, gx, gy, fx, fy);

            float t = clat1(cTseed, P.cw, gx, gy, fx, fy);
            float3 d3 = f3(0.f, 0.f, 0.f);
            for (int it = 0; it < RD_NEWTON_ITERS; ++it) {
                float fi = (t - P.tabT0) / P.tabDt;
                fi = fminf(fmaxf(fi, 0.0f), float(P.tabN - 2));
                const int i = int(fi);
                const float w = fi - i;
                const float3 p = orbP[i] + w * (orbP[i + 1] - orbP[i]);
                const float3 v = orbV[i] + w * (orbV[i + 1] - orbV[i]);
                const float3 a = orbA[i] + w * (orbA[i + 1] - orbA[i]);
                d3 = p - T;
                const float f  = dot3(d3, v);
                const float fp = dot3(v, v) + dot3(d3, a);
                if (fp != 0.0f) t -= f / fp;
            }

            const float srcY = t / P.dtAz;
            if (!(srcY >= 0.0f) || srcY >= float(P.nl - 1)) {
                rd_emit(0.0f, size_t(y) * P.W + x, cell, out, q);
                continue;
            }
            int si = 0;
            {
                float fi = (t - P.tabT0) / P.tabDt;
                fi = fminf(fmaxf(fi, 0.0f), float(P.tabN - 1));
                si = srgrIdx[int(fi)];
            }
            const SrgrDev& s0 = srgr[si];
            const SrgrDev& s1 = srgr[si + 1];

#if RD_SRGR_LUT

            const float sr = __fsqrt_rn(dot3(d3, d3));
            const float w1 = __saturatef((t - L.t0[si]) * L.invDt[si]);
            const float w0 = 1.0f - w1;
            const float sr0 = fmaf(w0, L.sr0[si], w1 * L.sr0[si + 1]);
            const float gr0 = fmaf(w0, L.gr0[si], w1 * L.gr0[si + 1]);
            const float dd  = sr - sr0;
            const float gr  = fmaf(w0, srgr_lut(L, si, dd),
                                   w1 * srgr_lut(L, si + 1, dd));
            const float srcX = (gr - gr0) / P.rgSpacing;
#elif RD_PROBE_NOPOLY

            const float srcX = 1.976f * (sqrtf(dot3(d3, d3)) - 799920.27f)
                             / P.rgSpacing;
#elif RD_FP32_SRGR

            const float sr = sqrtf(dot3(d3, d3));
            const float dtw = (s1.t > s0.t)
                            ? float((double(t) - s0.t) / (s1.t - s0.t)) : 0.0f;
            const float w1 = __saturatef(dtw), w0 = 1.0f - w1;

            const float sr0 = fmaf(w0, float(s0.sr0), w1 * float(s1.sr0));
            const float gr0 = fmaf(w0, float(s0.gr0), w1 * float(s1.gr0));
            const float dd  = sr - sr0;
            const int nc = max(s0.nc, s1.nc);
            float gr = 0.0f;
            for (int k = nc - 1; k >= 0; --k) {
                const float ck = fmaf(w0, (k < s0.nc ? float(s0.c[k]) : 0.0f),
                                      w1 * (k < s1.nc ? float(s1.c[k]) : 0.0f));
                gr = fmaf(gr, dd, ck);
            }
            const float srcX = (gr - gr0) / P.rgSpacing;
#else
            const double sr = sqrt(double(dot3(d3, d3)));
            const double dtw = (s1.t > s0.t) ? (double(t) - s0.t) / (s1.t - s0.t) : 0.0;
            const double w1 = dtw < 0.0 ? 0.0 : (dtw > 1.0 ? 1.0 : dtw), w0 = 1.0 - w1;

            const double sr0 = w0 * s0.sr0 + w1 * s1.sr0;
            const double gr0 = w0 * s0.gr0 + w1 * s1.gr0;
            const double dd = sr - sr0;
            double gr = 0.0, pw = 1.0;
            const int nc = max(s0.nc, s1.nc);
            for (int k = 0; k < nc; ++k) {
                const double ck = w0 * (k < s0.nc ? s0.c[k] : 0.0)
                                + w1 * (k < s1.nc ? s1.c[k] : 0.0);
                gr += ck * pw;
                pw *= dd;
            }
            const float srcX = float((gr - gr0) / P.rgSpacing);
#endif

            float val = 0.0f;
            const int ix = int(floorf(srcX)), iy = int(floorf(srcY));
            if (ix >= 0 && iy >= 0 && ix < P.ns - 1 && iy < P.nl - 1) {
                const float tx = srcX - ix, ty = srcY - iy;
#ifdef RD_SKIP_SOURCE_READ
                val = tx + ty;
#else
                const __half* r0 = sigma0 + size_t(iy) * P.ns;
                const __half* r1 = r0 + P.ns;
                const float a = __half2float(r0[ix]), b = __half2float(r0[ix + 1]);
                const float c = __half2float(r1[ix]), dv = __half2float(r1[ix + 1]);
                val = (1.0f - ty) * (a + tx * (b - a)) + ty * (c + tx * (dv - c));
#endif
            }
            rd_emit(val, size_t(y) * P.W + x, cell, out, q);
        }
    }
}

void launch_rd_geocode(const RdParams& prm,
                       const float3* cT0, const float3* cU, const float* cTseed,
                       const float2* cLonLat,
                       const float* dem,
                       const float3* orbP, const float3* orbV, const float3* orbA,
                       const SrgrDev* srgr, const int* srgrIdx,
                       const __half* sigma0, float* out, const RdQuant& q,
                       const SrgrLut& lut, cudaStream_t s) {

    dim3 block(RD_TILE, RD_TILE / 4);
    dim3 grid((prm.W + RD_TILE - 1) / RD_TILE, (prm.H + RD_TILE - 1) / RD_TILE);
    k_rd_geocode<<<grid, block, 0, s>>>(prm, cT0, cU, cTseed, cLonLat, dem,
                                      orbP, orbV, orbA, srgr, srgrIdx, sigma0,
                                      out, q, lut);
}
