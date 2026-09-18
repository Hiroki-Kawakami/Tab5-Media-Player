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

static void idct8_pass(int32_t *d, int step) {
    const int32_t a0 = d[0] + d[4 * step];
    const int32_t a2 = d[0] - d[4 * step];
    const int32_t a4 = (d[2 * step] >> 1) - d[6 * step];
    const int32_t a6 = (d[6 * step] >> 1) + d[2 * step];
    const int32_t b0 = a0 + a6;
    const int32_t b2 = a2 + a4;
    const int32_t b4 = a2 - a4;
    const int32_t b6 = a0 - a6;
    const int32_t a1 = -d[3 * step] + d[5 * step] - d[7 * step] - (d[7 * step] >> 1);
    const int32_t a3 = d[step] + d[7 * step] - d[3 * step] - (d[3 * step] >> 1);
    const int32_t a5 = -d[step] + d[7 * step] + d[5 * step] + (d[5 * step] >> 1);
    const int32_t a7 = d[3 * step] + d[5 * step] + d[step] + (d[step] >> 1);
    const int32_t b1 = (a7 >> 2) + a1;
    const int32_t b3 = a3 + (a5 >> 2);
    const int32_t b5 = (a3 >> 2) - a5;
    const int32_t b7 = a7 - (a1 >> 2);
    d[0] = b0 + b7;
    d[7 * step] = b0 - b7;
    d[step] = b2 + b5;
    d[6 * step] = b2 - b5;
    d[2 * step] = b4 + b3;
    d[5 * step] = b4 - b3;
    d[3 * step] = b6 + b1;
    d[4 * step] = b6 - b1;
}

void h264_idct8x8_add(uint8_t *dst, ptrdiff_t stride, const int16_t *block) {
    int32_t tmp[64];
    for (int i = 0; i < 64; i++) tmp[i] = block[i];
    for (int y = 0; y < 8; y++) idct8_pass(tmp + y * 8, 1);
    for (int x = 0; x < 8; x++) idct8_pass(tmp + x, 8);
    for (int y = 0; y < 8; y++) {
        uint8_t *out = dst + y * stride;
        const int32_t *row = tmp + y * 8;
        for (int x = 0; x < 8; x++) out[x] = clip_u8(out[x] + ((row[x] + 32) >> 6));
    }
}

void h264_luma_dc_dequant(int16_t *dc, int qp, int scale) {
    int tmp[16];
    for (int i = 0; i < 4; i++) {
        const int a = dc[i * 4 + 0], b = dc[i * 4 + 1], c = dc[i * 4 + 2], d = dc[i * 4 + 3];
        tmp[i * 4 + 0] = a + b + c + d;
        tmp[i * 4 + 1] = a + b - c - d;
        tmp[i * 4 + 2] = a - b - c + d;
        tmp[i * 4 + 3] = a - b + c - d;
    }
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

void h264_chroma_dc_dequant(int16_t *dc, int qp, int scale) {
    const int a = dc[0], b = dc[1], c = dc[2], d = dc[3];
    const int f[4] = { a + b + c + d, a - b + c - d, a + b - c - d, a - b - c + d };
    const int q6 = qp / 6;
    for (int i = 0; i < 4; i++) dc[i] = (int16_t)(((f[i] * scale) * (1 << q6)) >> 5);
}
