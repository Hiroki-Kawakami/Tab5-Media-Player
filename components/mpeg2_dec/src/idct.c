/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "mpeg2_internal.h"

enum {
    W1 = 22725,
    W2 = 21407,
    W3 = 19266,
    W4 = 16383,
    W5 = 12873,
    W6 = 8867,
    W7 = 4520,
    ROW_SHIFT = 11,
    COL_SHIFT = 20,
};

static inline void idct_row(int16_t *row) {
    if (!(row[1] | row[2] | row[3] | row[4] | row[5] | row[6] | row[7])) {
        const int16_t dc = (int16_t)(uint16_t)((uint32_t)row[0] << 3);
        for (int i = 0; i < 8; i++) row[i] = dc;
        return;
    }
    uint32_t a0 = (uint32_t)(W4 * row[0]) + (1u << (ROW_SHIFT - 1));
    uint32_t a1 = a0, a2 = a0, a3 = a0;
    a0 += (uint32_t)(W2 * row[2]);
    a1 += (uint32_t)(W6 * row[2]);
    a2 -= (uint32_t)(W6 * row[2]);
    a3 -= (uint32_t)(W2 * row[2]);
    uint32_t b0 = (uint32_t)(W1 * row[1] + W3 * row[3]);
    uint32_t b1 = (uint32_t)(W3 * row[1] - W7 * row[3]);
    uint32_t b2 = (uint32_t)(W5 * row[1] - W1 * row[3]);
    uint32_t b3 = (uint32_t)(W7 * row[1] - W5 * row[3]);
    if (row[4] | row[5] | row[6] | row[7]) {
        a0 += (uint32_t)(W4 * row[4] + W6 * row[6]);
        a1 += (uint32_t)(-W4 * row[4] - W2 * row[6]);
        a2 += (uint32_t)(-W4 * row[4] + W2 * row[6]);
        a3 += (uint32_t)(W4 * row[4] - W6 * row[6]);
        b0 += (uint32_t)(W5 * row[5] + W7 * row[7]);
        b1 += (uint32_t)(-W1 * row[5] - W5 * row[7]);
        b2 += (uint32_t)(W7 * row[5] + W3 * row[7]);
        b3 += (uint32_t)(W3 * row[5] - W1 * row[7]);
    }
    row[0] = (int16_t)((int32_t)(a0 + b0) >> ROW_SHIFT);
    row[7] = (int16_t)((int32_t)(a0 - b0) >> ROW_SHIFT);
    row[1] = (int16_t)((int32_t)(a1 + b1) >> ROW_SHIFT);
    row[6] = (int16_t)((int32_t)(a1 - b1) >> ROW_SHIFT);
    row[2] = (int16_t)((int32_t)(a2 + b2) >> ROW_SHIFT);
    row[5] = (int16_t)((int32_t)(a2 - b2) >> ROW_SHIFT);
    row[3] = (int16_t)((int32_t)(a3 + b3) >> ROW_SHIFT);
    row[4] = (int16_t)((int32_t)(a3 - b3) >> ROW_SHIFT);
}

static inline void idct_col(const int16_t *col, int out[8], uint8_t rows) {
    uint32_t a0 = (uint32_t)W4 * (uint32_t)(col[0] + ((1 << (COL_SHIFT - 1)) / W4));
    uint32_t a1 = a0, a2 = a0, a3 = a0;
    uint32_t b0 = 0, b1 = 0, b2 = 0, b3 = 0;
    if (rows & 0x04) {
        a0 += (uint32_t)(W2 * col[16]);
        a1 += (uint32_t)(W6 * col[16]);
        a2 -= (uint32_t)(W6 * col[16]);
        a3 -= (uint32_t)(W2 * col[16]);
    }
    if (rows & 0x02) {
        b0 = (uint32_t)(W1 * col[8]);
        b1 = (uint32_t)(W3 * col[8]);
        b2 = (uint32_t)(W5 * col[8]);
        b3 = (uint32_t)(W7 * col[8]);
    }
    if (rows & 0x08) {
        b0 += (uint32_t)(W3 * col[24]);
        b1 -= (uint32_t)(W7 * col[24]);
        b2 -= (uint32_t)(W1 * col[24]);
        b3 -= (uint32_t)(W5 * col[24]);
    }
    if (rows & 0x10) {
        a0 += (uint32_t)(W4 * col[32]);
        a1 -= (uint32_t)(W4 * col[32]);
        a2 -= (uint32_t)(W4 * col[32]);
        a3 += (uint32_t)(W4 * col[32]);
    }
    if (rows & 0x20) {
        b0 += (uint32_t)(W5 * col[40]);
        b1 -= (uint32_t)(W1 * col[40]);
        b2 += (uint32_t)(W7 * col[40]);
        b3 += (uint32_t)(W3 * col[40]);
    }
    if (rows & 0x40) {
        a0 += (uint32_t)(W6 * col[48]);
        a1 -= (uint32_t)(W2 * col[48]);
        a2 += (uint32_t)(W2 * col[48]);
        a3 -= (uint32_t)(W6 * col[48]);
    }
    if (rows & 0x80) {
        b0 += (uint32_t)(W7 * col[56]);
        b1 -= (uint32_t)(W5 * col[56]);
        b2 += (uint32_t)(W3 * col[56]);
        b3 -= (uint32_t)(W1 * col[56]);
    }
    out[0] = (int32_t)(a0 + b0) >> COL_SHIFT;
    out[1] = (int32_t)(a1 + b1) >> COL_SHIFT;
    out[2] = (int32_t)(a2 + b2) >> COL_SHIFT;
    out[3] = (int32_t)(a3 + b3) >> COL_SHIFT;
    out[4] = (int32_t)(a3 - b3) >> COL_SHIFT;
    out[5] = (int32_t)(a2 - b2) >> COL_SHIFT;
    out[6] = (int32_t)(a1 - b1) >> COL_SHIFT;
    out[7] = (int32_t)(a0 - b0) >> COL_SHIFT;
}

static void idct_rows(int16_t *blk, uint8_t rows) {
    for (int r = 0; r < 8; r++) {
        if (rows & (1u << r)) idct_row(blk + 8 * r);
    }
}

static void idct_put_c(uint8_t *dst, ptrdiff_t stride, int16_t *blk, uint8_t rows) {
    idct_rows(blk, rows);
    if (rows == 1) {
        for (int c = 0; c < 8; c++) {
            const uint8_t v = clip_u8((int32_t)((uint32_t)W4 * (uint32_t)(blk[c] + 32)) >> COL_SHIFT);
            for (int r = 0; r < 8; r++) dst[r * stride + c] = v;
        }
        return;
    }
    for (int c = 0; c < 8; c++) {
        int out[8];
        idct_col(blk + c, out, rows);
        for (int r = 0; r < 8; r++) dst[r * stride + c] = clip_u8(out[r]);
    }
}

static void idct_add_c(uint8_t *dst, ptrdiff_t stride, int16_t *blk, uint8_t rows) {
    idct_rows(blk, rows);
    if (rows == 1) {
        for (int c = 0; c < 8; c++) {
            const int v = (int32_t)((uint32_t)W4 * (uint32_t)(blk[c] + 32)) >> COL_SHIFT;
            for (int r = 0; r < 8; r++) dst[r * stride + c] = clip_u8(dst[r * stride + c] + v);
        }
        return;
    }
    for (int c = 0; c < 8; c++) {
        int out[8];
        idct_col(blk + c, out, rows);
        for (int r = 0; r < 8; r++) dst[r * stride + c] = clip_u8(dst[r * stride + c] + out[r]);
    }
}

#ifdef MPEG2_DEC_PIE
void mpeg2_k_idct_put(uint8_t *dst, ptrdiff_t stride, int16_t *blk, uint32_t masks);
void mpeg2_k_idct_add(uint8_t *dst, ptrdiff_t stride, int16_t *blk, uint32_t masks);
#endif

void mpeg2_idct_put(uint8_t *dst, ptrdiff_t stride, int16_t *blk, uint8_t rows, uint8_t rows_ac) {
#ifdef MPEG2_DEC_PIE
    if (rows != 1) {
        mpeg2_k_idct_put(dst, stride, blk, rows | ((uint32_t)rows_ac << 8));
        return;
    }
#else
    (void)rows_ac;
#endif
    idct_put_c(dst, stride, blk, rows);
    memset(blk, 0, 64 * sizeof(int16_t));
}

void mpeg2_idct_add(uint8_t *dst, ptrdiff_t stride, int16_t *blk, uint8_t rows, uint8_t rows_ac) {
#ifdef MPEG2_DEC_PIE
    if (rows != 1) {
        mpeg2_k_idct_add(dst, stride, blk, rows | ((uint32_t)rows_ac << 8));
        return;
    }
#else
    (void)rows_ac;
#endif
    idct_add_c(dst, stride, blk, rows);
    memset(blk, 0, 64 * sizeof(int16_t));
}

int mpeg2_dec_idct_selftest(uint32_t seed, int iterations, int max_level) {
#ifdef MPEG2_DEC_PIE
    int16_t blk_a[64] __attribute__((aligned(16)));
    int16_t blk_b[64] __attribute__((aligned(16)));
    uint8_t pix_a[16 * 8] __attribute__((aligned(16)));
    uint8_t pix_b[16 * 8] __attribute__((aligned(16)));
    int bad = 0;
    uint32_t x = seed ? seed : 1;
    for (int it = 0; it < iterations; it++) {
        memset(blk_a, 0, sizeof(blk_a));
        uint8_t rows = 0, rows_ac = 0;
        x = x * 1664525u + 1013904223u;
        const int count = 1 + (int)((x >> 8) % 12);
        const int range = (x >> 20) & 1 ? max_level : 64;
        for (int n = 0; n < count; n++) {
            x = x * 1664525u + 1013904223u;
            const int j = (int)((x >> 10) & 63);
            const int v = (int)((x >> 16) % (2u * range + 1)) - range;
            blk_a[j] = (int16_t)v;
        }
        if (it & 1) blk_a[63] ^= 1;
        for (int j = 0; j < 64; j++) {
            if (!blk_a[j]) continue;
            rows |= (uint8_t)(1u << (j >> 3));
            if (j & 7) rows_ac |= (uint8_t)(1u << (j >> 3));
        }
        for (int i = 0; i < 16 * 8; i++) {
            x = x * 1664525u + 1013904223u;
            pix_a[i] = pix_b[i] = (uint8_t)(x >> 24);
        }
        memcpy(blk_b, blk_a, sizeof(blk_a));
        const bool add = (it >> 1) & 1;
        if (add) {
            idct_add_c(pix_a, 16, blk_a, rows);
            mpeg2_k_idct_add(pix_b, 16, blk_b, rows | ((uint32_t)rows_ac << 8));
        } else {
            idct_put_c(pix_a, 16, blk_a, rows);
            mpeg2_k_idct_put(pix_b, 16, blk_b, rows | ((uint32_t)rows_ac << 8));
        }
        bool zero = true;
        for (int j = 0; j < 64; j++) zero &= blk_b[j] == 0;
        if (memcmp(pix_a, pix_b, sizeof(pix_a)) || !zero) bad++;
    }
    return bad;
#else
    (void)seed;
    (void)iterations;
    (void)max_level;
    return -1;
#endif
}
