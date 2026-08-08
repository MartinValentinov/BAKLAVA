#pragma once
#include <cuda_runtime.h>
#include <cstdint>
#include "detection.hpp"

// ---------------------------------------------------------------- tile triage
// Per-block maximum over a `bs` x `bs` grid. Used to identify tiles that are
// entirely out-of-swath (sigma0 == 0) so they never reach the network.
void launch_block_max(const float* scene, int W, int H, int bs,
                      int bw, int bh, float* out, cudaStream_t s);

// ---------------------------------------------------------------- preprocess
// Cut `n` tiles at `origins` out of the scene and write an NCHW float batch.
//
// Radiometry, matching the training render exactly:
//     g   = clamp((10*log10(max(sigma0,eps)) - db_lo) / (db_hi - db_lo), 0, 1)
//     u8  = round(g * 255)          <- the 8-bit quantisation the model saw
//     out = u8 / 255                <- Ultralytics' own scaling
// The grey value is replicated across all three channels, as in the RGB JPEGs
// the model was trained on.
void launch_preprocess(const float* scene, int W, int H,
                       const int2* origins, int n, int tile,
                       float db_lo, float db_hi,
                       float* out, cudaStream_t s);

// ---------------------------------------------------------------- decode
// YOLOv8-OBB head: [B, 6, A] = cx, cy, w, h, class_score, angle.
// Boxes arrive in input-tile pixels; this shifts them into scene pixels.
//
// Detections whose centre lies in the outer `overlap/2` margin of a tile are
// dropped unless that margin is the scene edge: the same vessel is seen more
// centrally by the neighbouring tile, so this removes duplicates at the source
// instead of leaning entirely on NMS.
void launch_decode(const float* trtOut, int B, int A,
                   const int2* origins, float conf, int tile, int overlap,
                   int W, int H, Det* dets, int* counter, int maxDet,
                   cudaStream_t s);

// ---------------------------------------------------------------- rotated NMS
// O(N^2) rotated-IoU bitmask, exact convex polygon intersection.
// N is at most a few thousand per scene, so this is microseconds.
void launch_rotated_nms_mask(const Det* dets, int n, float iouThr,
                             unsigned long long* mask, int colBlocks,
                             cudaStream_t s);

// ---------------------------------------------------------------- render
// Full-resolution debug image: same transfer function as the network input,
// grey replicated into interleaved RGB.
void launch_render_rgb(const float* scene, int W, int H,
                       float db_lo, float db_hi, uint8_t* rgb, cudaStream_t s);

// Draw oriented boxes as red outlines, `thickness` px wide.
void launch_draw_boxes(uint8_t* rgb, int W, int H,
                       const Det* dets, int n, int thickness, cudaStream_t s);
