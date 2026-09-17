/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_internal.h"

void h264_idct4x4_add(uint8_t *dst, ptrdiff_t stride, int16_t *block) {
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        const int16_t *d = block + i * 4;
        const int e = d[0] + d[2];
        const int f = d[0] - d[2];
        const int g = (d[1] >> 1) - d[3];
        const int h = d[1] + (d[3] >> 1);
        tmp[i * 4 + 0] = e + h;
        tmp[i * 4 + 1] = f + g;
        tmp[i * 4 + 2] = f - g;
        tmp[i * 4 + 3] = e - h;
    }
    for (int j = 0; j < 4; j++) {
        const int e = tmp[j] + tmp[8 + j];
        const int f = tmp[j] - tmp[8 + j];
        const int g = (tmp[4 + j] >> 1) - tmp[12 + j];
        const int h = tmp[4 + j] + (tmp[12 + j] >> 1);
        dst[j] = clip_u8(dst[j] + ((e + h + 32) >> 6));
        dst[stride + j] = clip_u8(dst[stride + j] + ((f + g + 32) >> 6));
        dst[2 * stride + j] = clip_u8(dst[2 * stride + j] + ((f - g + 32) >> 6));
        dst[3 * stride + j] = clip_u8(dst[3 * stride + j] + ((e - h + 32) >> 6));
    }
}

void h264_luma_dc_dequant(int16_t *dc, int qp) {
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        const int a = dc[i * 4 + 0], b = dc[i * 4 + 1], c = dc[i * 4 + 2], d = dc[i * 4 + 3];
        tmp[i * 4 + 0] = a + b + c + d;
        tmp[i * 4 + 1] = a + b - c - d;
        tmp[i * 4 + 2] = a - b - c + d;
        tmp[i * 4 + 3] = a - b + c - d;
    }
    const int scale = 16 * h264_dequant4[qp % 6][0];
    const int q6 = qp / 6;
    for (int j = 0; j < 4; j++) {
        const int a = tmp[j], b = tmp[4 + j], c = tmp[8 + j], d = tmp[12 + j];
        const int f[4] = { a + b + c + d, a + b - c - d, a - b - c + d, a - b + c - d };
        for (int i = 0; i < 4; i++) {
            int v;
            if (qp >= 36) v = (f[i] * scale) * (1 << (q6 - 6));
            else v = (f[i] * scale + (1 << (5 - q6))) >> (6 - q6);
            dc[i * 4 + j] = (int16_t)v;
        }
    }
}

void h264_chroma_dc_dequant(int16_t *dc, int qp) {
    const int a = dc[0], b = dc[1], c = dc[2], d = dc[3];
    const int f[4] = { a + b + c + d, a - b + c - d, a + b - c - d, a - b - c + d };
    const int scale = 16 * h264_dequant4[qp % 6][0];
    const int q6 = qp / 6;
    for (int i = 0; i < 4; i++) dc[i] = (int16_t)(((f[i] * scale) * (1 << q6)) >> 5);
}
