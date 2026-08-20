#pragma once
#include <cuda_runtime.h>
#include <cstdint>
#include "../common/detection.hpp"

void launch_render_rgb(const uint8_t* gray, int W, int H,
                       uint8_t* rgb, cudaStream_t s);

void launch_overview_rgb(const uint8_t* gray, int W, int H, int f,
                         uint8_t* rgb, int ow, int oh, cudaStream_t s);

void launch_draw_boxes(uint8_t* rgb, int W, int H,
                       const Det* dets, int n, int thickness, float scale,
                       cudaStream_t s);

// Draws one heading arrow per detection: shaft along the box's major axis
// toward the bow (per dirSign, from launch_estimate_heading), poking past
// the box edge, with a small arrowhead at the tip.
void launch_draw_heading_arrows(uint8_t* rgb, int W, int H,
                                const Det* dets, const float* dirSign, int n,
                                int thickness, float scale, cudaStream_t s);
