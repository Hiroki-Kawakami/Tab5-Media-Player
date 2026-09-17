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
        for (int x = 0; x < 16; x++) dst[x] = (int16_t)tap6(src + x, 1);
    }
}

void h264_k_avg_u8(const uint8_t *a, const uint8_t *b, uint8_t *dst, int rows, ptrdiff_t a_stride,
                   ptrdiff_t b_stride, ptrdiff_t dst_stride) {
    for (int r = 0; r < rows; r++, a += a_stride, b += b_stride, dst += dst_stride) {
        for (int x = 0; x < 16; x++) dst[x] = (uint8_t)((a[x] + b[x] + 1) >> 1);
    }
}

void h264_k_bilinear(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst, ptrdiff_t dst_stride,
                     int rows, int fx, int fy) {
    for (int r = 0; r < rows; r++, src += src_stride, dst += dst_stride) {
        for (int x = 0; x < 8; x++) {
            const int h0 = 8 * src[x] + fx * (src[x + 1] - src[x]);
            const int h1 = 8 * src[x + src_stride] + fx * (src[x + src_stride + 1] - src[x + src_stride]);
            dst[x] = (uint8_t)((8 * h0 + fy * (h1 - h0) + 32) >> 6);
        }
    }
}

void h264_k_pack(const uint8_t *c, const uint8_t *y, uint8_t *dst, int blocks) {
    for (int n = 0; n < blocks * 16; n++, dst += 3, y += 2) {
        dst[0] = c[n];
        dst[1] = y[0];
        dst[2] = y[1];
    }
}

void h264_k_unpack(const uint8_t *src, uint8_t *c, uint8_t *y, int blocks) {
    for (int n = 0; n < blocks * 16; n++, src += 3, y += 2) {
        c[n] = src[0];
        y[0] = src[1];
        y[1] = src[2];
    }
}

void h264_k_unpack_rows(const uint8_t *src, ptrdiff_t src_stride, uint8_t *c, ptrdiff_t c_stride,
                        uint8_t *y, ptrdiff_t y_stride, int rows) {
    for (int r = 0; r < rows; r++, src += src_stride, c += c_stride, y += y_stride) {
        h264_k_unpack(src, c, y, 1);
    }
}

void h264_k_prepare(void) {}

#endif
