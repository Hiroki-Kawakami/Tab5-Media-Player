/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_internal.h"

const uint8_t h264_clz8[256] = {
    8, 7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4,
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

const uint8_t h264_zigzag4x4[16] = {
    0, 1, 4, 8, 5, 2, 3, 6, 9, 12, 13, 10, 7, 11, 14, 15,
};

const uint8_t h264_chroma_qp[52] = {
    0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12, 13, 14, 15, 16, 17,
    18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 29, 30, 31, 32, 32, 33,
    34, 34, 35, 35, 36, 36, 37, 37, 37, 38, 38, 38, 39, 39, 39, 39,
};

const uint8_t h264_dequant4[6][16] = {
    { 10, 13, 10, 13, 13, 16, 13, 16, 10, 13, 10, 13, 13, 16, 13, 16 },
    { 11, 14, 11, 14, 14, 18, 14, 18, 11, 14, 11, 14, 14, 18, 14, 18 },
    { 13, 16, 13, 16, 16, 20, 16, 20, 13, 16, 13, 16, 16, 20, 16, 20 },
    { 14, 18, 14, 18, 18, 23, 18, 23, 14, 18, 14, 18, 18, 23, 18, 23 },
    { 16, 20, 16, 20, 20, 25, 20, 25, 16, 20, 16, 20, 20, 25, 20, 25 },
    { 18, 23, 18, 23, 23, 29, 23, 29, 18, 23, 18, 23, 23, 29, 23, 29 },
};

const uint8_t h264_col_corner[4] = { 0, 3, 12, 15 };

const uint8_t h264_zigzag8x8[64] = {
    0, 1, 8, 16, 9, 2, 3, 10, 17, 24, 32, 25, 18, 11, 4, 5,
    12, 19, 26, 33, 40, 48, 41, 34, 27, 20, 13, 6, 7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36, 29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46, 53, 60, 61, 54, 47, 55, 62, 63
};

const uint8_t h264_dequant8[6][64] = {
    {
        20, 19, 25, 19, 20, 19, 25, 19, 19, 18, 24, 18, 19, 18, 24, 18,
        25, 24, 32, 24, 25, 24, 32, 24, 19, 18, 24, 18, 19, 18, 24, 18,
        20, 19, 25, 19, 20, 19, 25, 19, 19, 18, 24, 18, 19, 18, 24, 18,
        25, 24, 32, 24, 25, 24, 32, 24, 19, 18, 24, 18, 19, 18, 24, 18
    },
    {
        22, 21, 28, 21, 22, 21, 28, 21, 21, 19, 26, 19, 21, 19, 26, 19,
        28, 26, 35, 26, 28, 26, 35, 26, 21, 19, 26, 19, 21, 19, 26, 19,
        22, 21, 28, 21, 22, 21, 28, 21, 21, 19, 26, 19, 21, 19, 26, 19,
        28, 26, 35, 26, 28, 26, 35, 26, 21, 19, 26, 19, 21, 19, 26, 19
    },
    {
        26, 24, 33, 24, 26, 24, 33, 24, 24, 23, 31, 23, 24, 23, 31, 23,
        33, 31, 42, 31, 33, 31, 42, 31, 24, 23, 31, 23, 24, 23, 31, 23,
        26, 24, 33, 24, 26, 24, 33, 24, 24, 23, 31, 23, 24, 23, 31, 23,
        33, 31, 42, 31, 33, 31, 42, 31, 24, 23, 31, 23, 24, 23, 31, 23
    },
    {
        28, 26, 35, 26, 28, 26, 35, 26, 26, 25, 33, 25, 26, 25, 33, 25,
        35, 33, 45, 33, 35, 33, 45, 33, 26, 25, 33, 25, 26, 25, 33, 25,
        28, 26, 35, 26, 28, 26, 35, 26, 26, 25, 33, 25, 26, 25, 33, 25,
        35, 33, 45, 33, 35, 33, 45, 33, 26, 25, 33, 25, 26, 25, 33, 25
    },
    {
        32, 30, 40, 30, 32, 30, 40, 30, 30, 28, 38, 28, 30, 28, 38, 28,
        40, 38, 51, 38, 40, 38, 51, 38, 30, 28, 38, 28, 30, 28, 38, 28,
        32, 30, 40, 30, 32, 30, 40, 30, 30, 28, 38, 28, 30, 28, 38, 28,
        40, 38, 51, 38, 40, 38, 51, 38, 30, 28, 38, 28, 30, 28, 38, 28
    },
    {
        36, 34, 46, 34, 36, 34, 46, 34, 34, 32, 43, 32, 34, 32, 43, 32,
        46, 43, 58, 43, 46, 43, 58, 43, 34, 32, 43, 32, 34, 32, 43, 32,
        36, 34, 46, 34, 36, 34, 46, 34, 34, 32, 43, 32, 34, 32, 43, 32,
        46, 43, 58, 43, 46, 43, 58, 43, 34, 32, 43, 32, 34, 32, 43, 32
    }
};

const uint8_t h264_default_scaling4[2][16] = {
    { 6, 13, 20, 28, 13, 20, 28, 32, 20, 28, 32, 37, 28, 32, 37, 42 },
    { 10, 14, 20, 24, 14, 20, 24, 27, 20, 24, 27, 30, 24, 27, 30, 34 },
};

const uint8_t h264_default_scaling8[2][64] = {
    {
        6, 10, 13, 16, 18, 23, 25, 27, 10, 11, 16, 18, 23, 25, 27, 29,
        13, 16, 18, 23, 25, 27, 29, 31, 16, 18, 23, 25, 27, 29, 31, 33,
        18, 23, 25, 27, 29, 31, 33, 36, 23, 25, 27, 29, 31, 33, 36, 38,
        25, 27, 29, 31, 33, 36, 38, 40, 27, 29, 31, 33, 36, 38, 40, 42,
    },
    {
        9, 13, 15, 17, 19, 21, 22, 24, 13, 13, 17, 19, 21, 22, 24, 25,
        15, 17, 19, 21, 22, 24, 25, 27, 17, 19, 21, 22, 24, 25, 27, 28,
        19, 21, 22, 24, 25, 27, 28, 30, 21, 22, 24, 25, 27, 28, 30, 32,
        22, 24, 25, 27, 28, 30, 32, 33, 24, 25, 27, 28, 30, 32, 33, 35,
    },
};

static const uint8_t kCoeffTokenLen[4][4 * 17] = {
    {
        1,  0,  0,  0,  6,  2,  0,  0,  8,  6,  3,  0,  9,  8,  7,  5,
        10, 9,  8,  6,  11, 10, 9,  7,  13, 11, 10, 8,  13, 13, 11, 9,
        13, 13, 13, 10, 14, 14, 13, 11, 14, 14, 14, 13, 15, 15, 14, 14,
        15, 15, 15, 14, 16, 15, 15, 15, 16, 16, 16, 15, 16, 16, 16, 16,
        16, 16, 16, 16,
    },
    {
        2,  0,  0,  0,  6,  2,  0,  0,  6,  5,  3,  0,  7,  6,  6,  4,
        8,  6,  6,  4,  8,  7,  7,  5,  9,  8,  8,  6,  11, 9,  9,  6,
        11, 11, 11, 7,  12, 11, 11, 9,  12, 12, 12, 11, 12, 12, 12, 11,
        13, 13, 13, 12, 13, 13, 13, 13, 13, 14, 13, 13, 14, 14, 14, 13,
        14, 14, 14, 14,
    },
    {
        4,  0,  0,  0,  6,  4,  0,  0,  6,  5,  4,  0,  6,  5,  5,  4,
        7,  5,  5,  4,  7,  5,  5,  4,  7,  6,  6,  4,  7,  6,  6,  4,
        8,  7,  7,  5,  8,  8,  7,  6,  9,  8,  8,  7,  9,  9,  8,  8,
        9,  9,  9,  8,  10, 9,  9,  9,  10, 10, 10, 10, 10, 10, 10, 10,
        10, 10, 10, 10,
    },
};

static const uint8_t kCoeffTokenCode[4][4 * 17] = {
    {
        1,  0,  0,  0,  5,  1,  0,  0,  7,  4,  1,  0,  7,  6,  5,  3,
        7,  6,  5,  3,  7,  6,  5,  4,  15, 6,  5,  4,  11, 14, 5,  4,
        8,  10, 13, 4,  15, 14, 9,  4,  11, 10, 13, 12, 15, 14, 9,  12,
        11, 10, 13, 8,  15, 1,  9,  12, 11, 14, 13, 8,  7,  10, 9,  12,
        4,  6,  5,  8,
    },
    {
        3,  0,  0,  0,  11, 2,  0,  0,  7,  7,  3,  0,  7,  10, 9,  5,
        7,  6,  5,  4,  4,  6,  5,  6,  7,  6,  5,  8,  15, 6,  5,  4,
        11, 14, 13, 4,  15, 10, 9,  4,  11, 14, 13, 12, 8,  10, 9,  8,
        15, 14, 13, 12, 11, 10, 9,  12, 7,  11, 6,  8,  9,  8,  10, 1,
        7,  6,  5,  4,
    },
    {
        15, 0,  0,  0,  15, 14, 0,  0,  11, 15, 13, 0,  8,  12, 14, 12,
        15, 10, 11, 11, 11, 8,  9,  10, 9,  14, 13, 9,  8,  10, 9,  8,
        15, 14, 13, 13, 11, 14, 10, 12, 15, 10, 13, 12, 11, 14, 9,  12,
        8,  10, 13, 8,  13, 7,  9,  12, 9,  12, 11, 10, 5,  8,  7,  6,
        1,  4,  3,  2,
    },
};

static const uint8_t kCoeffTokenDcLen[4 * 5] = {
    2, 0, 0, 0, 6, 1, 0, 0, 6, 6, 3, 0, 6, 7, 7, 6, 6, 8, 8, 7,
};

static const uint8_t kCoeffTokenDcCode[4 * 5] = {
    1, 0, 0, 0, 7, 1, 0, 0, 4, 6, 1, 0, 3, 3, 2, 5, 2, 3, 2, 0,
};

static const uint8_t kTotalZerosLen[15][16] = {
    { 1, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 9 },
    { 3, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 6, 6, 6, 6 },
    { 4, 3, 3, 3, 4, 4, 3, 3, 4, 5, 5, 6, 5, 6 },
    { 5, 3, 4, 4, 3, 3, 3, 4, 3, 4, 5, 5, 5 },
    { 4, 4, 4, 3, 3, 3, 3, 3, 4, 5, 4, 5 },
    { 6, 5, 3, 3, 3, 3, 3, 3, 4, 3, 6 },
    { 6, 5, 3, 3, 3, 2, 3, 4, 3, 6 },
    { 6, 4, 5, 3, 2, 2, 3, 3, 6 },
    { 6, 6, 4, 2, 2, 3, 2, 5 },
    { 5, 5, 3, 2, 2, 2, 4 },
    { 4, 4, 3, 3, 1, 3 },
    { 4, 4, 2, 1, 3 },
    { 3, 3, 1, 2 },
    { 2, 2, 1 },
    { 1, 1 },
};

static const uint8_t kTotalZerosCode[15][16] = {
    { 1, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 3, 2, 1 },
    { 7, 6, 5, 4, 3, 5, 4, 3, 2, 3, 2, 3, 2, 1, 0 },
    { 5, 7, 6, 5, 4, 3, 4, 3, 2, 3, 2, 1, 1, 0 },
    { 3, 7, 5, 4, 6, 5, 4, 3, 3, 2, 2, 1, 0 },
    { 5, 4, 3, 7, 6, 5, 4, 3, 2, 1, 1, 0 },
    { 1, 1, 7, 6, 5, 4, 3, 2, 1, 1, 0 },
    { 1, 1, 5, 4, 3, 3, 2, 1, 1, 0 },
    { 1, 1, 1, 3, 3, 2, 2, 1, 0 },
    { 1, 0, 1, 3, 2, 1, 1, 1 },
    { 1, 0, 1, 3, 2, 1, 1 },
    { 0, 1, 1, 2, 1, 3 },
    { 0, 1, 1, 1, 1 },
    { 0, 1, 1, 1 },
    { 0, 1, 1 },
    { 0, 1 },
};

static const uint8_t kTotalZerosDcLen[3][4] = {
    { 1, 2, 3, 3 },
    { 1, 2, 2 },
    { 1, 1 },
};

static const uint8_t kTotalZerosDcCode[3][4] = {
    { 1, 1, 1, 0 },
    { 1, 1, 0 },
    { 1, 0 },
};

static const uint8_t kRunLen[7][16] = {
    { 1, 1 },
    { 1, 2, 2 },
    { 2, 2, 2, 2 },
    { 2, 2, 2, 3, 3 },
    { 2, 2, 3, 3, 3, 3 },
    { 2, 3, 3, 3, 3, 3, 3 },
    { 3, 3, 3, 3, 3, 3, 3, 4, 5, 6, 7, 8, 9, 10, 11 },
};

static const uint8_t kRunCode[7][16] = {
    { 1, 0 },
    { 1, 1, 0 },
    { 3, 2, 1, 0 },
    { 3, 2, 1, 1, 0 },
    { 3, 2, 3, 2, 1, 0 },
    { 3, 0, 1, 3, 2, 5, 4 },
    { 7, 6, 5, 4, 3, 2, 1, 1, 1, 1, 1, 1, 1, 1, 1 },
};

#define VLC_ROOT_BITS 8
#define VLC_INVALID_ENTRY ((int16_t)(1023 << 5))

typedef struct {
    const uint8_t *len;
    const uint8_t *code;
    uint16_t count;
    uint8_t max_len;
} vlc_spec_t;

static size_t vlc_table_size(const vlc_spec_t *spec, uint8_t root) {
    size_t size = (size_t)1 << root;
    if (spec->max_len <= root) return size;
    for (uint32_t prefix = 0; prefix < (1u << root); prefix++) {
        uint8_t deepest = 0;
        for (uint16_t s = 0; s < spec->count; s++) {
            const uint8_t len = spec->len[s];
            if (len <= root) continue;
            if ((uint32_t)(spec->code[s] >> (len - root)) != prefix) continue;
            if (len - root > deepest) deepest = (uint8_t)(len - root);
        }
        if (deepest) size += 1 + ((size_t)1 << deepest);
    }
    return size;
}

static bool vlc_build(struct h264_dec *dec, vlc_t *vlc, const vlc_spec_t *spec) {
    const uint8_t root = spec->max_len < VLC_ROOT_BITS ? spec->max_len : VLC_ROOT_BITS;
    const size_t size = vlc_table_size(spec, root);
    int16_t *table = h264_work_alloc(dec, size * sizeof(int16_t));
    if (!table) return false;
    for (size_t i = 0; i < size; i++) table[i] = VLC_INVALID_ENTRY;
    size_t next = (size_t)1 << root;

    for (uint32_t prefix = 0; prefix < (1u << root); prefix++) {
        uint8_t deepest = 0;
        for (uint16_t s = 0; s < spec->count; s++) {
            const uint8_t len = spec->len[s];
            if (len <= root) continue;
            if ((uint32_t)(spec->code[s] >> (len - root)) != prefix) continue;
            if (len - root > deepest) deepest = (uint8_t)(len - root);
        }
        if (!deepest) continue;
        table[prefix] = (int16_t)-(int)next;
        table[next] = deepest;
        for (uint16_t s = 0; s < spec->count; s++) {
            const uint8_t len = spec->len[s];
            if (len <= root) continue;
            if ((uint32_t)(spec->code[s] >> (len - root)) != prefix) continue;
            const uint8_t rest = (uint8_t)(len - root);
            const uint32_t low = spec->code[s] & ((1u << rest) - 1);
            const uint32_t first = low << (deepest - rest);
            for (uint32_t k = 0; k < (1u << (deepest - rest)); k++) {
                table[next + 1 + first + k] = (int16_t)((s << 5) | rest);
            }
        }
        next += 1 + ((size_t)1 << deepest);
    }

    for (uint16_t s = 0; s < spec->count; s++) {
        const uint8_t len = spec->len[s];
        if (!len || len > root) continue;
        const uint32_t first = (uint32_t)spec->code[s] << (root - len);
        for (uint32_t k = 0; k < (1u << (root - len)); k++) {
            table[first + k] = (int16_t)((s << 5) | len);
        }
    }
    vlc->table = table;
    vlc->root_bits = root;
    return true;
}

static uint8_t max_of(const uint8_t *v, uint16_t n) {
    uint8_t m = 0;
    for (uint16_t i = 0; i < n; i++) m = v[i] > m ? v[i] : m;
    return m;
}

bool h264_build_vlcs(struct h264_dec *dec) {
    static uint8_t flc_len[4 * 17];
    static uint8_t flc_code[4 * 17];
    for (int t = 0; t <= 16; t++) {
        for (int o = 0; o < 4; o++) {
            const int s = t * 4 + o;
            flc_len[s] = o <= t ? 6 : 0;
            flc_code[s] = (uint8_t)(t == 0 ? 3 : ((t - 1) << 2) | o);
        }
    }

    for (int i = 0; i < 4; i++) {
        vlc_spec_t spec;
        if (i < 3) {
            spec = (vlc_spec_t){ kCoeffTokenLen[i], kCoeffTokenCode[i], 4 * 17, 0 };
        } else {
            spec = (vlc_spec_t){ flc_len, flc_code, 4 * 17, 6 };
        }
        spec.max_len = max_of(spec.len, spec.count);
        if (!vlc_build(dec, &dec->coeff_token[i], &spec)) return false;
    }
    vlc_spec_t dc = { kCoeffTokenDcLen, kCoeffTokenDcCode, 4 * 5, 0 };
    dc.max_len = max_of(dc.len, dc.count);
    if (!vlc_build(dec, &dec->coeff_token_dc, &dc)) return false;

    for (int i = 0; i < 15; i++) {
        vlc_spec_t spec = { kTotalZerosLen[i], kTotalZerosCode[i], (uint16_t)(16 - i), 0 };
        spec.max_len = max_of(spec.len, spec.count);
        if (!vlc_build(dec, &dec->total_zeros[i], &spec)) return false;
    }
    for (int i = 0; i < 3; i++) {
        vlc_spec_t spec = { kTotalZerosDcLen[i], kTotalZerosDcCode[i], (uint16_t)(4 - i), 0 };
        spec.max_len = max_of(spec.len, spec.count);
        if (!vlc_build(dec, &dec->total_zeros_dc[i], &spec)) return false;
    }
    for (int i = 0; i < 7; i++) {
        vlc_spec_t spec = { kRunLen[i], kRunCode[i], (uint16_t)(i < 6 ? i + 2 : 15), 0 };
        spec.max_len = max_of(spec.len, spec.count);
        if (!vlc_build(dec, &dec->run_before[i], &spec)) return false;
    }
    return true;
}
