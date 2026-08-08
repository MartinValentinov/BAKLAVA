#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <cuda_runtime.h>

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
        std::fprintf(stderr, "[fatal] ");                                      \
        std::fprintf(stderr, __VA_ARGS__);                                     \
        std::fprintf(stderr, "\n");                                            \
        std::exit(1);                                                          \
    } while (0)

// Wall-clock stage timer. Every stage prints, so a missed latency target is
// immediately attributable rather than a mystery.
class Timer {
public:
    explicit Timer(std::string label) : label_(std::move(label)),
                                        t0_(std::chrono::steady_clock::now()) {}
    double ms() const {
        auto t1 = std::chrono::steady_clock::now();
        return std::chrono::duration<double, std::milli>(t1 - t0_).count();
    }
    void report() const {
        std::fprintf(stderr, "[time] %-28s %8.1f ms\n", label_.c_str(), ms());
    }
    ~Timer() = default;
private:
    std::string label_;
    std::chrono::steady_clock::time_point t0_;
};
