/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_internal.h"

void h264_cabac_init(cabac_t *c, bits_t *b, int qp, int model) {
    const int8_t (*init)[2] = model < 0 ? h264_cabac_init_i : h264_cabac_init_pb[model];
    const int sqp = clip3(0, 51, qp);
    for (int i = 0; i < CABAC_CONTEXTS; i++) {
        const int pre = clip3(1, 126, ((init[i][0] * sqp) >> 4) + init[i][1]);
        c->state[i] = pre <= 63 ? (uint8_t)((63 - pre) << 1) : (uint8_t)(((pre - 64) << 1) | 1);
    }
    c->bits = b;
    c->range = 510;
    c->offset = bits_u(b, 9);
}

void h264_cabac_reinit(cabac_t *c) {
    c->range = 510;
    c->offset = bits_u(c->bits, 9);
}

static inline void renorm(cabac_t *c) {
    while (c->range < 256) {
        c->range <<= 1;
        c->offset = (c->offset << 1) | bits_u1(c->bits);
    }
}

int h264_cabac_decision(cabac_t *c, int ctx) {
    uint8_t *state = &c->state[ctx];
    const int s = *state >> 1;
    const int mps = *state & 1;
    const uint32_t lps = h264_cabac_range_lps[s][(c->range >> 6) & 3];
    c->range -= lps;
    int bin;
    if (c->offset >= c->range) {
        bin = !mps;
        c->offset -= c->range;
        c->range = lps;
        *state = (uint8_t)((h264_cabac_trans_lps[s] << 1) | (s == 0 ? !mps : mps));
    } else {
        bin = mps;
        *state = (uint8_t)((h264_cabac_trans_mps[s] << 1) | mps);
    }
    renorm(c);
    return bin;
}

int h264_cabac_bypass(cabac_t *c) {
    c->offset = (c->offset << 1) | bits_u1(c->bits);
    if (c->offset >= c->range) {
        c->offset -= c->range;
        return 1;
    }
    return 0;
}

int h264_cabac_terminate(cabac_t *c) {
    c->range -= 2;
    if (c->offset >= c->range) return 1;
    renorm(c);
    return 0;
}

int h264_cabac_mb_type_i(cabac_t *c, int base, int inc, bool intra_slice) {
    int ctx = base + inc;
    if (!h264_cabac_decision(c, ctx)) return 0;
    if (intra_slice) base += 2;
    if (h264_cabac_terminate(c)) return 25;
    int type = 1;
    type += 12 * h264_cabac_decision(c, base + 1);
    if (h264_cabac_decision(c, base + 2)) {
        type += 4 + 4 * h264_cabac_decision(c, base + 2 + (intra_slice ? 1 : 0));
    }
    type += 2 * h264_cabac_decision(c, base + 3 + (intra_slice ? 1 : 0));
    type += h264_cabac_decision(c, base + 3 + (intra_slice ? 2 : 0));
    return type;
}

int h264_cabac_mb_type_p(cabac_t *c) {
    if (h264_cabac_decision(c, 14)) return 5 + h264_cabac_mb_type_i(c, 17, 0, false);
    if (h264_cabac_decision(c, 15)) return 2 - h264_cabac_decision(c, 17);
    return 3 * h264_cabac_decision(c, 16);
}

int h264_cabac_mb_type_b(cabac_t *c, int inc) {
    if (!h264_cabac_decision(c, 27 + inc)) return 0;
    if (!h264_cabac_decision(c, 27 + 3)) return 1 + h264_cabac_decision(c, 27 + 5);
    int bits = h264_cabac_decision(c, 27 + 4) << 3;
    bits |= h264_cabac_decision(c, 27 + 5) << 2;
    bits |= h264_cabac_decision(c, 27 + 5) << 1;
    bits |= h264_cabac_decision(c, 27 + 5);
    if (bits < 8) return bits + 3;
    if (bits == 13) return 23 + h264_cabac_mb_type_i(c, 32, 0, false);
    if (bits == 14) return 11;
    if (bits == 15) return 22;
    bits = (bits << 1) | h264_cabac_decision(c, 27 + 5);
    return bits - 4;
}

int h264_cabac_sub_type_p(cabac_t *c) {
    if (h264_cabac_decision(c, 21)) return 0;
    if (!h264_cabac_decision(c, 22)) return 1;
    if (h264_cabac_decision(c, 23)) return 2;
    return 3;
}

int h264_cabac_sub_type_b(cabac_t *c) {
    if (!h264_cabac_decision(c, 36)) return 0;
    if (!h264_cabac_decision(c, 37)) return 1 + h264_cabac_decision(c, 39);
    int type = 3;
    if (h264_cabac_decision(c, 38)) {
        if (h264_cabac_decision(c, 39)) return 11 + h264_cabac_decision(c, 39);
        type += 4;
    }
    type += 2 * h264_cabac_decision(c, 39);
    type += h264_cabac_decision(c, 39);
    return type;
}

int h264_cabac_ref(cabac_t *c, int inc) {
    int ref = 0;
    int ctx = inc;
    while (h264_cabac_decision(c, 54 + ctx)) {
        ref++;
        ctx = (ctx >> 2) + 4;
        if (ref >= MAX_REFS) break;
    }
    return ref;
}

int h264_cabac_mvd(cabac_t *c, int base, int amvd) {
    if (!h264_cabac_decision(c, base + (amvd > 2) + (amvd > 32))) return 0;
    int mvd = 1;
    int ctx = base + 3;
    while (mvd < 9 && h264_cabac_decision(c, ctx)) {
        if (mvd < 4) ctx++;
        mvd++;
    }
    if (mvd >= 9) {
        int k = 3;
        while (h264_cabac_bypass(c) && k < 24) {
            mvd += 1 << k;
            k++;
        }
        while (k--) mvd += h264_cabac_bypass(c) << k;
    }
    return h264_cabac_bypass(c) ? -mvd : mvd;
}

int h264_cabac_intra_pred_mode(cabac_t *c, int pred) {
    if (h264_cabac_decision(c, 68)) return pred;
    int mode = h264_cabac_decision(c, 69);
    mode += 2 * h264_cabac_decision(c, 69);
    mode += 4 * h264_cabac_decision(c, 69);
    return mode + (mode >= pred);
}

int h264_cabac_chroma_mode(cabac_t *c, int inc) {
    if (!h264_cabac_decision(c, 64 + inc)) return 0;
    if (!h264_cabac_decision(c, 64 + 3)) return 1;
    if (!h264_cabac_decision(c, 64 + 3)) return 2;
    return 3;
}

int h264_cabac_cbp_luma(cabac_t *c, int left, int top) {
    int cbp = 0;
    int ctx = !(left & 0x02) + 2 * !(top & 0x04);
    cbp += h264_cabac_decision(c, 73 + ctx);
    ctx = !(cbp & 0x01) + 2 * !(top & 0x08);
    cbp += h264_cabac_decision(c, 73 + ctx) << 1;
    ctx = !(left & 0x08) + 2 * !(cbp & 0x01);
    cbp += h264_cabac_decision(c, 73 + ctx) << 2;
    ctx = !(cbp & 0x04) + 2 * !(cbp & 0x02);
    cbp += h264_cabac_decision(c, 73 + ctx) << 3;
    return cbp;
}

int h264_cabac_cbp_chroma(cabac_t *c, int left, int top) {
    int ctx = (left > 0) + 2 * (top > 0);
    if (!h264_cabac_decision(c, 77 + ctx)) return 0;
    ctx = 4 + (left == 2) + 2 * (top == 2);
    return 1 + h264_cabac_decision(c, 77 + ctx);
}

int h264_cabac_qp_delta(cabac_t *c, bool prev_nonzero) {
    if (!h264_cabac_decision(c, 60 + (prev_nonzero ? 1 : 0))) return 0;
    int value = 1;
    int ctx = 2;
    while (h264_cabac_decision(c, 60 + ctx) && value <= 102) {
        ctx = 3;
        value++;
    }
    return (value & 1) ? (value + 1) >> 1 : -((value + 1) >> 1);
}

int h264_cabac_transform8x8(cabac_t *c, int inc) {
    return h264_cabac_decision(c, 399 + inc);
}

static const uint16_t kSigBase[6] = { 105 + 0, 105 + 15, 105 + 29, 105 + 44, 105 + 47, 402 };
static const uint16_t kLastBase[6] = { 166 + 0, 166 + 15, 166 + 29, 166 + 44, 166 + 47, 417 };
static const uint16_t kLevelBase[6] = { 227 + 0, 227 + 10, 227 + 20, 227 + 30, 227 + 39, 426 };
static const uint16_t kCbfBase[5] = { 85, 89, 93, 97, 101 };
static const uint8_t kLevel1Ctx[8] = { 1, 2, 3, 4, 0, 0, 0, 0 };
static const uint8_t kLevelGt1Ctx[8] = { 5, 5, 5, 5, 6, 7, 8, 9 };
static const uint8_t kLevelNext[2][8] = {
    { 1, 2, 3, 3, 4, 5, 6, 7 },
    { 4, 4, 4, 4, 5, 6, 7, 7 },
};

int h264_cabac_cbf(cabac_t *c, int cat, int inc) {
    return h264_cabac_decision(c, kCbfBase[cat] + inc);
}

int h264_cabac_residual(cabac_t *c, int cat, int max_coeff, int16_t *levels, uint8_t *positions) {
    const uint16_t sig = kSigBase[cat];
    const uint16_t last_base = kLastBase[cat];
    const uint16_t level_base = kLevelBase[cat];
    uint8_t index[64];
    int count = 0;
    int last = 0;

    if (cat == 5) {
        for (last = 0; last < 63; last++) {
            if (h264_cabac_decision(c, sig + h264_sig_coeff_offset_8x8[last])) {
                index[count++] = (uint8_t)last;
                if (h264_cabac_decision(c, last_base + h264_last_coeff_offset_8x8[last])) {
                    last = max_coeff;
                    break;
                }
            }
        }
    } else {
        for (last = 0; last < max_coeff - 1; last++) {
            if (h264_cabac_decision(c, sig + last)) {
                index[count++] = (uint8_t)last;
                if (h264_cabac_decision(c, last_base + last)) {
                    last = max_coeff;
                    break;
                }
            }
        }
    }
    if (last == max_coeff - 1) index[count++] = (uint8_t)last;

    const int total = count;
    int node = 0;
    int out = 0;
    while (count) {
        const int pos = index[--count];
        int level;
        if (!h264_cabac_decision(c, level_base + kLevel1Ctx[node])) {
            node = kLevelNext[0][node];
            level = 1;
        } else {
            int abs = 2;
            const int ctx = level_base + kLevelGt1Ctx[node];
            node = kLevelNext[1][node];
            while (abs < 15 && h264_cabac_decision(c, ctx)) abs++;
            if (abs >= 15) {
                int k = 0;
                while (h264_cabac_bypass(c) && k < 23) k++;
                abs = 1;
                while (k--) abs += abs + h264_cabac_bypass(c);
                abs += 14;
            }
            level = abs;
        }
        if (h264_cabac_bypass(c)) level = -level;
        levels[out] = (int16_t)level;
        positions[out] = (uint8_t)pos;
        out++;
    }
    return total;
}
