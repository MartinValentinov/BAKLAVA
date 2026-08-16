#pragma once
#include <cuda_runtime.h>
#include <cstdint>
#include "../common/detection.hpp"

void launch_quantise(const float* scene, int W, int H,
                     float db_lo, float db_hi, int decim,
                     uint8_t* gray, uint8_t* dataMask, unsigned int* maxGrid,
                     unsigned int* sumGrid, int cw, int ch, cudaStream_t s);

void launch_preprocess(const uint8_t* gray, int W, int H,
                       const int2* origins, int n, int tile,
                       float* out, cudaStream_t s);

void launch_decode(const float* trtOut, int B, int A,
                   const int2* origins, float conf, int tile, int overlap,
                   int W, int H, Det* dets, int* counter, int maxDet,
                   cudaStream_t s);

void launch_rotated_nms_mask(const Det* dets, int n, float iouThr,
                             unsigned long long* mask, int colBlocks,
                             cudaStream_t s);
