#pragma once
#include "common.h"

void homography_square_to_quad(
    float x0, float y0,
    float x1, float y1,
    float x2, float y2,
    float x3, float y3,
    float H[9]
);

void apply_homography(const float H[9], float u, float v, float* x, float* y);
