#pragma once
#include <string>
#include <cstdint>
#include <cuda_runtime.h>

// Encodes an interleaved RGB device buffer to a baseline JPEG on disk.
//
// At 29508 x 21739 the buffer is ~1.9 GB and the file is well inside JPEG's
// 65535-px dimension limit. If nvJPEG refuses the size, this falls back to
// libjpeg via GDAL on the host rather than silently producing nothing.
bool encodeJpegRGB(const uint8_t* dRGB, int W, int H, int quality,
                   const std::string& path, cudaStream_t stream);
