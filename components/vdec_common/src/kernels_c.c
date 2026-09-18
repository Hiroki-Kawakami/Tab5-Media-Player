/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include <string.h>
#include "vdec_kernels.h"

#ifndef VDEC_PIE

void vdec_k_avg_u8(const uint8_t *a, const uint8_t *b, uint8_t *dst, int rows, ptrdiff_t a_stride,
                   ptrdiff_t b_stride, ptrdiff_t dst_stride) {
    for (int r = 0; r < rows; r++, a += a_stride, b += b_stride, dst += dst_stride) {
        for (int x = 0; x < 16; x++) dst[x] = (uint8_t)((a[x] + b[x] + 1) >> 1);
    }
}

void vdec_k_bilinear(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst, ptrdiff_t dst_stride,
                     int rows, int fx, int fy) {
    for (int r = 0; r < rows; r++, src += src_stride, dst += dst_stride) {
        for (int x = 0; x < 8; x++) {
            const int h0 = 8 * src[x] + fx * (src[x + 1] - src[x]);
            const int h1 = 8 * src[x + src_stride] + fx * (src[x + src_stride + 1] - src[x + src_stride]);
            dst[x] = (uint8_t)((8 * h0 + fy * (h1 - h0) + 32) >> 6);
        }
    }
}

void vdec_k_copy(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst, ptrdiff_t dst_stride,
                 int rows) {
    for (int r = 0; r < rows; r++, src += src_stride, dst += dst_stride) memcpy(dst, src, 16);
}

void vdec_k_pack(const uint8_t *c, const uint8_t *y, uint8_t *dst, int blocks) {
    for (int n = 0; n < blocks * 16; n++, dst += 3, y += 2) {
        dst[0] = c[n];
        dst[1] = y[0];
        dst[2] = y[1];
    }
}

void vdec_k_unpack(const uint8_t *src, uint8_t *c, uint8_t *y, int blocks) {
    for (int n = 0; n < blocks * 16; n++, src += 3, y += 2) {
        c[n] = src[0];
        y[0] = src[1];
        y[1] = src[2];
    }
}

void vdec_k_unpack_rows(const uint8_t *src, ptrdiff_t src_stride, uint8_t *c, ptrdiff_t c_stride,
                        uint8_t *y, ptrdiff_t y_stride, int rows) {
    for (int r = 0; r < rows; r++, src += src_stride, c += c_stride, y += y_stride) {
        vdec_k_unpack(src, c, y, 1);
    }
}

void vdec_k_prepare(void) {}


#endif
