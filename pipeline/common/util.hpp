#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
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
        char _m[1024];                                                         \
        std::snprintf(_m, sizeof _m, __VA_ARGS__);                             \
        std::fprintf(stderr, "[fatal] %s\n", _m);                              \
        throw std::runtime_error(_m);                                          \
    } while (0)

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
