/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "kernels.h"
#include "h264_internal.h"

#ifndef H264_DEC_PIE

static inline int tap6(const uint8_t *p, ptrdiff_t s) {
    return p[0] + p[5 * s] - 5 * (p[s] + p[4 * s]) + 20 * (p[2 * s] + p[3 * s]);
}

void h264_k_tap6_u8(const uint8_t *src, ptrdiff_t step, uint8_t *dst, int rows,
                    ptrdiff_t src_stride, ptrdiff_t dst_stride) {
    for (int r = 0; r < rows; r++, src += src_stride, dst += dst_stride) {
        for (int x = 0; x < 16; x++) dst[x] = clip_u8((tap6(src + x, step) + 16) >> 5);
    }
}

void h264_k_tap6_s16(const uint8_t *src, int16_t *dst, int rows, ptrdiff_t src_stride) {
    for (int r = 0; r < rows; r++, src += src_stride, dst += 16) {
        for (int x = 0; x < 16; x++) dst[x] = (int16_t)(tap6(src + x, 1) + 16);
    }
}

void h264_k_tap6v_s16(const int16_t *mid, uint8_t *dst, int rows, ptrdiff_t dst_stride) {
    for (int r = 0; r < rows; r++, mid += 16, dst += dst_stride) {
        for (int x = 0; x < 16; x++) {
            const int v = mid[x] + mid[x + 80] - 5 * (mid[x + 16] + mid[x + 64]) +
                         20 * (mid[x + 32] + mid[x + 48]);
            dst[x] = clip_u8(v >> 10);
        }
    }
}

void h264_k_weight_u8(uint8_t *dst, ptrdiff_t stride, int rows, int w, int round, int shift,
                      int offset) {
    for (int r = 0; r < rows; r++, dst += stride) {
        for (int x = 0; x < 16; x++) dst[x] = clip_u8(((dst[x] * w + round) >> shift) + offset);
    }
}

void h264_k_weight_bi_u8(const uint8_t *a, const uint8_t *b, uint8_t *dst, int rows,
                         ptrdiff_t src_stride, ptrdiff_t dst_stride, int w1) {
    for (int r = 0; r < rows; r++, a += src_stride, b += src_stride, dst += dst_stride) {
        for (int x = 0; x < 16; x++) {
            const int d = ((b[x] - a[x]) * w1 + 32) >> 6;
            dst[x] = clip_u8(a[x] + d);
        }
    }
}

#endif
