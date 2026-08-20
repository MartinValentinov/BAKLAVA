#pragma once
#include <string>
#include <cstdint>
#include <cuda_runtime.h>

bool encodeJpegRGB(const uint8_t* dRGB, int W, int H, int quality,
                   const std::string& path, cudaStream_t stream);
