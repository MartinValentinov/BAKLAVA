#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include <string>
#include <cuda_runtime.h>
#include <nvtx3/nvToolsExt.h>

#define CUDA_CHECK(x)                                                          \
    do {                                                                       \
        cudaError_t _e = (x);                                                  \
        if (_e != cudaSuccess) {                                               \
            std::fprintf(stderr, "[cuda] %s:%d %s -> %s\n", __FILE__, __LINE__,\
                         #x, cudaGetErrorString(_e));                          \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)

#define FATAL(...)                                                             \
    do {                                                                       \
        char _m[1024];                                                         \
        std::snprintf(_m, sizeof _m, __VA_ARGS__);                             \
        std::fprintf(stderr, "[fatal] %s\n", _m);                              \
        throw std::runtime_error(_m);                                          \
    } while (0)

class Timer {
public:
    explicit Timer(std::string label) : label_(std::move(label)),
                                        t0_(std::chrono::steady_clock::now()) {
        pushNvtx();
    }
    double ms() const {
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0_).count();
    }
    void report() const {
        std::fprintf(stderr, "[time] %-28s %8.1f ms\n", label_.c_str(), ms());
        popNvtx();
    }
    ~Timer() { popNvtx(); }
private:
    std::string label_;
    std::chrono::steady_clock::time_point t0_;
    mutable bool popped_ = false;

    void popNvtx() const {
        if (popped_) return;
        popped_ = true;
        nvtxRangePop();
    }

    void pushNvtx() const {
        static constexpr uint32_t kPalette[] = {
            0xFF3399FFu, 0xFFFF6633u, 0xFF33CC66u, 0xFFCC33FFu,
            0xFFFFCC00u, 0xFFFF3366u, 0xFF33CCCCu, 0xFF999999u,
            0xFF66CC33u, 0xFFFF9933u, 0xFF3366FFu, 0xFFCC6633u,
        };
        uint32_t h = 2166136261u;
        for (unsigned char c : label_) { h ^= c; h *= 16777619u; }
        const uint32_t color = kPalette[h % (sizeof(kPalette) / sizeof(kPalette[0]))];

        nvtxEventAttributes_t attr = {};
        attr.version     = NVTX_VERSION;
        attr.size        = NVTX_EVENT_ATTRIB_STRUCT_SIZE;
        attr.colorType   = NVTX_COLOR_ARGB;
        attr.color       = color;
        attr.messageType = NVTX_MESSAGE_TYPE_ASCII;
        attr.message.ascii = label_.c_str();
        nvtxRangePushEx(&attr);
    }
};
