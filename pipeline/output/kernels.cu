#include "kernels.cuh"

#define CDIV(a, b) (((a) + (b) - 1) / (b))

__global__ void k_render_rgb(const uint8_t* __restrict__ gray, int W, int H,
                             uint8_t* __restrict__ rgb) {
    const long long total = (long long)W * H;
    for (long long i = blockIdx.x * (long long)blockDim.x + threadIdx.x;
         i < total; i += (long long)gridDim.x * blockDim.x) {
        const uint8_t g = gray[i];
        rgb[i * 3 + 0] = g;
        rgb[i * 3 + 1] = g;
        rgb[i * 3 + 2] = g;
    }
}

void launch_render_rgb(const uint8_t* gray, int W, int H,
                       uint8_t* rgb, cudaStream_t s) {
    k_render_rgb<<<4096, 256, 0, s>>>(gray, W, H, rgb);
}

__global__ void k_overview_rgb(const uint8_t* __restrict__ gray, int W, int H, int f,
                               uint8_t* __restrict__ rgb, int ow, int oh) {
    const long long total = (long long)ow * oh;
    for (long long i = blockIdx.x * (long long)blockDim.x + threadIdx.x;
         i < total; i += (long long)gridDim.x * blockDim.x) {
        const int ox = int(i % ow), oy = int(i / ow);
        const int x0 = ox * f, y0 = oy * f;
        const int x1 = min(x0 + f, W), y1 = min(y0 + f, H);

        unsigned sum = 0, cnt = 0;
        for (int y = y0; y < y1; ++y) {
            const uint8_t* row = gray + size_t(y) * W;
            for (int x = x0; x < x1; ++x) { sum += row[x]; ++cnt; }
        }
        const uint8_t v = cnt ? (uint8_t)((sum + cnt / 2) / cnt) : 0;
        rgb[i * 3 + 0] = v;
        rgb[i * 3 + 1] = v;
        rgb[i * 3 + 2] = v;
    }
}

void launch_overview_rgb(const uint8_t* gray, int W, int H, int f,
                         uint8_t* rgb, int ow, int oh, cudaStream_t s) {
    k_overview_rgb<<<2048, 256, 0, s>>>(gray, W, H, f, rgb, ow, oh);
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

__global__ void k_draw_boxes(uint8_t* __restrict__ rgb, int W, int H,
                             const Det* __restrict__ dets, int n, int th,
                             float scale) {
    const int id = blockIdx.x * blockDim.x + threadIdx.x;
    if (id >= n * 4) return;
    const int di = id / 4, ei = id % 4;

    Pt c[4];
    rbox_corners(dets[di], c);
#pragma unroll
    for (int i = 0; i < 4; ++i) { c[i].x *= scale; c[i].y *= scale; }
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
                rgb[o + 0] = 255;
                rgb[o + 1] = 0;
                rgb[o + 2] = 0;
            }
        }
    }
}

void launch_draw_boxes(uint8_t* rgb, int W, int H,
                       const Det* dets, int n, int thickness, float scale,
                       cudaStream_t s) {
    if (n <= 0) return;
    const int threads = 128;
    k_draw_boxes<<<CDIV(n * 4, threads), threads, 0, s>>>(rgb, W, H, dets, n,
                                                          thickness, scale);
}

__device__ __forceinline__ void draw_seg(uint8_t* __restrict__ rgb, int W, int H,
                                         float ax, float ay, float bx, float by,
                                         int r, uint8_t cr, uint8_t cg, uint8_t cb) {
    const float dx = bx - ax, dy = by - ay;
    const float len = sqrtf(dx * dx + dy * dy);
    const int steps = max(1, (int)(len * 2.0f));
    for (int s = 0; s <= steps; ++s) {
        const float t = (float)s / steps;
        const int px = __float2int_rn(ax + dx * t);
        const int py = __float2int_rn(ay + dy * t);
        for (int oy = -r; oy <= r; ++oy) {
            for (int ox = -r; ox <= r; ++ox) {
                const int x = px + ox, y = py + oy;
                if (x < 0 || x >= W || y < 0 || y >= H) continue;
                const size_t o = (size_t(y) * W + x) * 3;
                rgb[o + 0] = cr;
                rgb[o + 1] = cg;
                rgb[o + 2] = cb;
            }
        }
    }
}

__global__ void k_draw_arrows(uint8_t* __restrict__ rgb, int W, int H,
                              const Det* __restrict__ dets,
                              const float* __restrict__ dirSign,
                              int n, int th, float scale) {
    const int i = blockIdx.x * blockDim.x + threadIdx.x;
    if (i >= n) return;

    const Det d = dets[i];
    const float ca = __cosf(d.angle), sa = __sinf(d.angle);
    const float u0x = (d.w >= d.h) ? ca : -sa;
    const float u0y = (d.w >= d.h) ? sa :  ca;
    const float vx = -u0y, vy = u0x;

    const float sgn = dirSign[i];
    const float ux = u0x * sgn, uy = u0y * sgn;

    const float L    = fmaxf(d.w, d.h);
    const float half = L * 0.5f;
    const float tailLen = fminf(half, L * 0.35f);
    const float headLen = fmaxf(6.0f, fminf(half * 0.6f, 40.0f));

    const float tailX = d.cx - ux * tailLen;
    const float tailY = d.cy - uy * tailLen;
    const float tipX  = d.cx + ux * (half + headLen * 0.6f);
    const float tipY  = d.cy + uy * (half + headLen * 0.6f);

    const int r = max(1, th / 2);
    const uint8_t cr = 255, cg = 255, cb = 0;   // yellow, distinct from the red box

    draw_seg(rgb, W, H, tailX * scale, tailY * scale, tipX * scale, tipY * scale,
            r, cr, cg, cb);

    constexpr float HEAD_ANGLE = 0.5236f;   // 30 degrees
    const float ca2 = cosf(HEAD_ANGLE), sa2 = sinf(HEAD_ANGLE);
    const float bx = -ux, by = -uy;
    const float h1x = bx * ca2 + vx * sa2, h1y = by * ca2 + vy * sa2;
    const float h2x = bx * ca2 - vx * sa2, h2y = by * ca2 - vy * sa2;

    draw_seg(rgb, W, H, tipX * scale, tipY * scale,
            (tipX + h1x * headLen) * scale, (tipY + h1y * headLen) * scale,
            r, cr, cg, cb);
    draw_seg(rgb, W, H, tipX * scale, tipY * scale,
            (tipX + h2x * headLen) * scale, (tipY + h2y * headLen) * scale,
            r, cr, cg, cb);
}

void launch_draw_heading_arrows(uint8_t* rgb, int W, int H,
                                const Det* dets, const float* dirSign, int n,
                                int thickness, float scale, cudaStream_t s) {
    if (n <= 0) return;
    const int threads = 128;
    k_draw_arrows<<<CDIV(n, threads), threads, 0, s>>>(rgb, W, H, dets, dirSign, n,
                                                        thickness, scale);
}
