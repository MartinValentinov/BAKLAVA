#include "kernels.cuh"
#include "common/quantise.cuh"
#include <algorithm>
#include <cfloat>
#include <cmath>

#define CDIV(a, b) (((a) + (b) - 1) / (b))

using quant::sigma_to_u8;

__global__ void k_quantise(const float* __restrict__ scene, int W, int H,
                           float lo, float inv_span, int decim, int shift,
                           uint8_t* __restrict__ gray,
                           uint8_t* dataMask, unsigned int* maxGrid,
                           unsigned int* sumGrid, int cw) {
    for (int y = blockIdx.y; y < H; y += gridDim.y) {
        const float* __restrict__ srow = scene + size_t(y) * W;
        uint8_t* __restrict__ grow = gray + size_t(y) * W;
        const size_t crow = size_t(shift >= 0 ? (y >> shift) : (y / decim)) * cw;
        uint8_t* mrow = dataMask + crow;
        unsigned int* xrow = maxGrid + crow;
        unsigned int* srow2 = sumGrid + crow;
        for (int x = blockIdx.x * blockDim.x + threadIdx.x; x < W;
             x += gridDim.x * blockDim.x) {
            const float v = srow[x];
            const uint8_t g = sigma_to_u8(v, lo, inv_span);
            grow[x] = g;
            const int cx = (shift >= 0 ? (x >> shift) : (x / decim));
            if (v > 0.0f) {
                uint8_t* p = mrow + cx;
                if (!*p) *p = 1;
            }
            if (g && g > xrow[cx]) atomicMax(xrow + cx, (unsigned int)g);
            atomicAdd(srow2 + cx, (unsigned int)g);
        }
    }
}

void launch_quantise(const float* scene, int W, int H,
                     float db_lo, float db_hi, int decim,
                     uint8_t* gray, uint8_t* dataMask, unsigned int* maxGrid,
                     unsigned int* sumGrid, int cw, int ch, cudaStream_t s) {
    (void)ch;
    const float span = quant::inv_span(db_lo, db_hi);
    const int shift = quant::decim_shift(decim);

    dim3 grid((W + 255) / 256, H);
    k_quantise<<<grid, 256, 0, s>>>(scene, W, H, db_lo, span, decim, shift,
                                    gray, dataMask, maxGrid, sumGrid, cw);
}

__global__ void k_preprocess(const uint8_t* __restrict__ gray, int W, int H,
                             const int2* __restrict__ origins, int n, int tile,
                             float* __restrict__ out) {
    const int t = blockIdx.z;
    if (t >= n) return;

    const int2 o = origins[t];
    const size_t plane = size_t(tile) * tile;
    float* __restrict__ obase = out + size_t(t) * 3 * plane;

    for (int ty = blockIdx.y; ty < tile; ty += gridDim.y) {
        const int y = o.y + ty;
        const bool rowOk = (y >= 0 && y < H);
        const uint8_t* __restrict__ srow = rowOk ? gray + size_t(y) * W : nullptr;

        for (int tx = blockIdx.x * blockDim.x + threadIdx.x; tx < tile;
             tx += gridDim.x * blockDim.x) {
            const int x = o.x + tx;
            uint8_t u = 0;
            if (rowOk && x >= 0 && x < W) u = srow[x];
            const float g = u * (1.0f / 255.0f);

            const size_t off = size_t(ty) * tile + tx;
            obase[off]             = g;
            obase[off + plane]     = g;
            obase[off + 2 * plane] = g;
        }
    }
}

void launch_preprocess(const uint8_t* gray, int W, int H,
                       const int2* origins, int n, int tile,
                       float* out, cudaStream_t s) {
    if (n <= 0) return;
    const int threads = 256;
    dim3 grid(CDIV(tile, threads), std::min(tile, 64), n);
    k_preprocess<<<grid, threads, 0, s>>>(gray, W, H, origins, n, tile, out);
}

__global__ void k_decode(const float* __restrict__ o, int B, int A,
                         const int2* __restrict__ origins, float conf,
                         int tile, int overlap, int W, int H,
                         Det* __restrict__ dets, int* __restrict__ counter, int maxDet) {
    const long long total = (long long)B * A;
    for (long long idx = blockIdx.x * (long long)blockDim.x + threadIdx.x;
         idx < total; idx += (long long)gridDim.x * blockDim.x) {
        const int b = int(idx / A);
        const int a = int(idx % A);

        const size_t base = size_t(b) * 6 * A + a;
        const float score = o[base + 4 * A];
        if (score < conf) continue;

        const float cx = o[base];
        const float cy = o[base + A];

        const int2 org = origins[b];
        const float m = overlap * 0.5f;
        if (org.x > 0            && cx < m)          continue;
        if (org.x + tile < W     && cx > tile - m)   continue;
        if (org.y > 0            && cy < m)          continue;
        if (org.y + tile < H     && cy > tile - m)   continue;

        const int slot = atomicAdd(counter, 1);
        if (slot >= maxDet) continue;

        Det d;
        d.cx    = cx + org.x;
        d.cy    = cy + org.y;
        d.w     = o[base + 2 * A];
        d.h     = o[base + 3 * A];
        d.angle = o[base + 5 * A];
        d.score = score;
        dets[slot] = d;
    }
}

void launch_decode(const float* trtOut, int B, int A,
                   const int2* origins, float conf, int tile, int overlap,
                   int W, int H, Det* dets, int* counter, int maxDet,
                   cudaStream_t s) {
    const long long total = (long long)B * A;
    const int threads = 256;
    int blocks = int(std::min<long long>(CDIV(total, threads), 65535));
    k_decode<<<blocks, threads, 0, s>>>(trtOut, B, A, origins, conf, tile, overlap,
                                        W, H, dets, counter, maxDet);
}

struct Pt { float x, y; };

__device__ __forceinline__ void rbox_corners(const Det& d, Pt* c) {
    const float ca = __cosf(d.angle), sa = __sinf(d.angle);
    const float hw = d.w * 0.5f, hh = d.h * 0.5f;
    const float ox[4] = {-hw,  hw, hw, -hw};
    const float oy[4] = {-hh, -hh, hh,  hh};
#pragma unroll
    for (int i = 0; i < 4; ++i) {
        c[i].x = d.cx + ox[i] * ca - oy[i] * sa;
        c[i].y = d.cy + ox[i] * sa + oy[i] * ca;
    }
}

__device__ __forceinline__ float poly_area(const Pt* p, int n) {
    float a = 0.0f;
    for (int i = 0, j = n - 1; i < n; j = i++)
        a += p[j].x * p[i].y - p[i].x * p[j].y;
    return fabsf(a) * 0.5f;
}

__device__ __forceinline__ int clip_edge(const Pt* sub, int n, Pt a, Pt b, Pt* out) {
    int m = 0;
    for (int i = 0, j = n - 1; i < n; j = i++) {
        const Pt P = sub[j], Q = sub[i];
        const float dp = (b.x - a.x) * (P.y - a.y) - (b.y - a.y) * (P.x - a.x);
        const float dq = (b.x - a.x) * (Q.y - a.y) - (b.y - a.y) * (Q.x - a.x);
        const bool inP = dp >= 0.0f, inQ = dq >= 0.0f;
        if (inP != inQ) {
            const float t = dp / (dp - dq);
            out[m].x = P.x + t * (Q.x - P.x);
            out[m].y = P.y + t * (Q.y - P.y);
            ++m;
        }
        if (inQ) { out[m++] = Q; }
        if (m >= 16) break;
    }
    return m;
}

__device__ float rbox_iou(const Det& A, const Det& B) {
    Pt ca[4], cb[4];
    rbox_corners(A, ca);
    rbox_corners(B, cb);

    auto orient = [](Pt* p) {
        float a = 0.0f;
        for (int i = 0, j = 3; i < 4; j = i++) a += p[j].x * p[i].y - p[i].x * p[j].y;
        if (a < 0.0f) { Pt t = p[1]; p[1] = p[3]; p[3] = t; }
    };
    orient(ca);
    orient(cb);

    Pt buf1[16], buf2[16];
    int n = 4;
    for (int i = 0; i < 4; ++i) buf1[i] = ca[i];

    for (int e = 0, f = 3; e < 4; f = e++) {
        n = clip_edge(buf1, n, cb[f], cb[e], buf2);
        if (n == 0) return 0.0f;
        for (int i = 0; i < n; ++i) buf1[i] = buf2[i];
    }

    const float inter = poly_area(buf1, n);
    const float ua = A.w * A.h + B.w * B.h - inter;
    return ua > 0.0f ? inter / ua : 0.0f;
}

static constexpr int NMS_BLOCK = 64;

__global__ void k_nms_mask(const Det* __restrict__ dets, int n, float thr,
                           unsigned long long* __restrict__ mask, int colBlocks) {
    const int row = blockIdx.y;
    const int col = blockIdx.x;

    const int rowSize = min(n - row * NMS_BLOCK, NMS_BLOCK);
    const int colSize = min(n - col * NMS_BLOCK, NMS_BLOCK);

    __shared__ Det sblk[NMS_BLOCK];
    if (threadIdx.x < colSize) sblk[threadIdx.x] = dets[col * NMS_BLOCK + threadIdx.x];
    __syncthreads();

    if (threadIdx.x < rowSize) {
        const int idx = row * NMS_BLOCK + threadIdx.x;
        const Det cur = dets[idx];
        unsigned long long t = 0ULL;
        const int start = (row == col) ? threadIdx.x + 1 : 0;
        for (int i = start; i < colSize; ++i)
            if (rbox_iou(cur, sblk[i]) > thr) t |= (1ULL << i);
        mask[size_t(idx) * colBlocks + col] = t;
    }
}

void launch_rotated_nms_mask(const Det* dets, int n, float iouThr,
                             unsigned long long* mask, int colBlocks,
                             cudaStream_t s) {
    dim3 grid(CDIV(n, NMS_BLOCK), CDIV(n, NMS_BLOCK));
    k_nms_mask<<<grid, NMS_BLOCK, 0, s>>>(dets, n, iouThr, mask, colBlocks);
}
