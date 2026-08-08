#include "kernels.cuh"
#include <algorithm>
#include <cfloat>
#include <cmath>

#define CDIV(a, b) (((a) + (b) - 1) / (b))
static constexpr float SIGMA_EPS = 1e-12f;

// =============================================================== tile triage

__global__ void k_block_max(const float* __restrict__ scene, int W, int H, int bs,
                            int bw, int bh, float* __restrict__ out) {
    const int blk = blockIdx.x;
    if (blk >= bw * bh) return;
    const int bx = (blk % bw) * bs;
    const int by = (blk / bw) * bs;

    float m = 0.0f;
    for (int i = threadIdx.x; i < bs * bs; i += blockDim.x) {
        const int x = bx + (i % bs);
        const int y = by + (i / bs);
        if (x < W && y < H) m = fmaxf(m, scene[size_t(y) * W + x]);
    }

    __shared__ float sm[256];
    sm[threadIdx.x] = m;
    __syncthreads();
    for (int s = blockDim.x / 2; s > 0; s >>= 1) {
        if (threadIdx.x < s) sm[threadIdx.x] = fmaxf(sm[threadIdx.x], sm[threadIdx.x + s]);
        __syncthreads();
    }
    if (threadIdx.x == 0) out[blk] = sm[0];
}

void launch_block_max(const float* scene, int W, int H, int bs,
                      int bw, int bh, float* out, cudaStream_t s) {
    k_block_max<<<bw * bh, 256, 0, s>>>(scene, W, H, bs, bw, bh, out);
}

// =============================================================== preprocess

__device__ __forceinline__ float sigma_to_unit(float v, float lo, float inv_span) {
    const float db = 10.0f * __log10f(fmaxf(v, SIGMA_EPS));
    float g = (db - lo) * inv_span;
    g = __saturatef(g);
    // Quantise to 8 bit, then rescale: reproduces the JPEG the model trained on.
    return __float2int_rn(g * 255.0f) * (1.0f / 255.0f);
}

__global__ void k_preprocess(const float* __restrict__ scene, int W, int H,
                             const int2* __restrict__ origins, int n, int tile,
                             float lo, float inv_span, float* __restrict__ out) {
    const long long total = (long long)n * tile * tile;
    for (long long idx = blockIdx.x * (long long)blockDim.x + threadIdx.x;
         idx < total; idx += (long long)gridDim.x * blockDim.x) {
        const int t  = int(idx / (tile * tile));
        const int r  = int(idx % (tile * tile));
        const int ty = r / tile;
        const int tx = r % tile;

        const int2 o = origins[t];
        const int x = o.x + tx;
        const int y = o.y + ty;

        float v = 0.0f;
        if (x >= 0 && x < W && y >= 0 && y < H) v = scene[size_t(y) * W + x];
        const float g = sigma_to_unit(v, lo, inv_span);

        const size_t plane = size_t(tile) * tile;
        const size_t base  = size_t(t) * 3 * plane + size_t(ty) * tile + tx;
        out[base]             = g;
        out[base + plane]     = g;
        out[base + 2 * plane] = g;
    }
}

void launch_preprocess(const float* scene, int W, int H,
                       const int2* origins, int n, int tile,
                       float db_lo, float db_hi, float* out, cudaStream_t s) {
    const float inv_span = 1.0f / fmaxf(db_hi - db_lo, 1e-6f);
    const long long total = (long long)n * tile * tile;
    const int threads = 256;
    int blocks = int(std::min<long long>(CDIV(total, threads), 65535));
    k_preprocess<<<blocks, threads, 0, s>>>(scene, W, H, origins, n, tile,
                                            db_lo, inv_span, out);
}

// =============================================================== decode

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

        // Drop detections sitting in a tile's overlap margin -- unless that
        // margin is the edge of the scene, where no neighbour exists.
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

// =============================================================== rotated NMS

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

// Sutherland-Hodgman clip of `sub` against the half-plane left of edge a->b.
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

    // Force counter-clockwise so the half-plane test has a consistent sign.
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

// =============================================================== render

__global__ void k_render_rgb(const float* __restrict__ scene, int W, int H,
                             float lo, float inv_span, uint8_t* __restrict__ rgb) {
    const long long total = (long long)W * H;
    for (long long i = blockIdx.x * (long long)blockDim.x + threadIdx.x;
         i < total; i += (long long)gridDim.x * blockDim.x) {
        const float db = 10.0f * __log10f(fmaxf(scene[i], SIGMA_EPS));
        const uint8_t g = (uint8_t)__float2int_rn(__saturatef((db - lo) * inv_span) * 255.0f);
        rgb[i * 3 + 0] = g;
        rgb[i * 3 + 1] = g;
        rgb[i * 3 + 2] = g;
    }
}

void launch_render_rgb(const float* scene, int W, int H,
                       float db_lo, float db_hi, uint8_t* rgb, cudaStream_t s) {
    const float inv_span = 1.0f / fmaxf(db_hi - db_lo, 1e-6f);
    k_render_rgb<<<4096, 256, 0, s>>>(scene, W, H, db_lo, inv_span, rgb);
}

// One thread per (detection, edge). Walks the edge in half-pixel steps and
// stamps a square of `thickness`. Overlapping writes are all the same colour,
// so the races are benign.
__global__ void k_draw_boxes(uint8_t* __restrict__ rgb, int W, int H,
                             const Det* __restrict__ dets, int n, int th) {
    const int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= n * 4) return;
    const int di = id / 4, ei = id % 4;

    Pt c[4];
    rbox_corners(dets[di], c);
    const Pt a = c[ei], b = c[(ei + 1) & 3];

    const float dx = b.x - a.x, dy = b.y - a.y;
    const float len = sqrtf(dx * dx + dy * dy);
    const int steps = max(1, (int)(len * 2.0f));
    const int r = th / 2;

    for (int s = 0; s <= steps; ++s) {
        const float t = (float)s / steps;
        const int px = __float2int_rn(a.x + dx * t);
        const int py = __float2int_rn(a.y + dy * t);
        for (int oy = -r; oy <= r; ++oy) {
            for (int ox = -r; ox <= r; ++ox) {
                const int x = px + ox, y = py + oy;
                if (x < 0 || x >= W || y < 0 || y >= H) continue;
                const size_t o = (size_t(y) * W + x) * 3;
                rgb[o + 0] = 255;   // red
                rgb[o + 1] = 0;
                rgb[o + 2] = 0;
            }
        }
    }
}

void launch_draw_boxes(uint8_t* rgb, int W, int H,
                       const Det* dets, int n, int thickness, cudaStream_t s) {
    if (n <= 0) return;
    const int threads = 128;
    k_draw_boxes<<<CDIV(n * 4, threads), threads, 0, s>>>(rgb, W, H, dets, n, thickness);
}
