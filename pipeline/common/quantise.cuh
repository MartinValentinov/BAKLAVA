#pragma once
#include <cuda_runtime.h>
#include <cmath>
#include <cstdint>

namespace quant {

constexpr float kSigmaEps = 1e-12f;

#ifdef __CUDACC__
__device__ __forceinline__ uint8_t sigma_to_u8(float v, float lo, float inv_span) {
    const float db = 10.0f * __log10f(fmaxf(v, kSigmaEps));
    const float g  = __saturatef((db - lo) * inv_span);
    return (uint8_t)__float2int_rn(g * 255.0f);
}
#endif

inline float inv_span(float db_lo, float db_hi) {
    return 1.0f / std::fmax(db_hi - db_lo, 1e-6f);
}

inline int decim_shift(int decim) {
    if (decim <= 0 || (decim & (decim - 1)) != 0) return -1;
    int s = 0;
    while ((1 << s) < decim) ++s;
    return s;
}

}
