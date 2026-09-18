/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_internal.h"

#define SHAPE_16x16 0
#define SHAPE_16x8 1
#define SHAPE_8x16 2
#define SHAPE_SUB 3

static const uint8_t kCbpIntra[48] = {
    47, 31, 15, 0,  23, 27, 29, 30, 7,  11, 13, 14, 39, 43, 45, 46,
    16, 3,  5,  10, 12, 19, 21, 26, 28, 35, 37, 42, 44, 1,  2,  4,
    8,  17, 18, 20, 24, 6,  9,  22, 25, 32, 33, 34, 36, 40, 38, 41,
};

static const uint8_t kCbpInter[48] = {
    0,  16, 1,  2,  4,  8,  32, 3,  5,  10, 12, 15, 47, 7,  11, 13,
    14, 6,  9,  31, 35, 37, 42, 44, 33, 34, 36, 40, 39, 43, 45, 46,
    17, 18, 20, 24, 19, 21, 26, 28, 23, 27, 29, 30, 22, 25, 38, 41,
};

static const uint8_t kBlockX[16] = { 0, 1, 0, 1, 2, 3, 2, 3, 0, 1, 0, 1, 2, 3, 2, 3 };
static const uint8_t kBlockY[16] = { 0, 0, 1, 1, 0, 0, 1, 1, 2, 2, 3, 3, 2, 2, 3, 3 };

static const uint8_t kZIndex[4][4] = {
    { 0, 1, 4, 5 },
    { 2, 3, 6, 7 },
    { 8, 9, 12, 13 },
    { 10, 11, 14, 15 },
};

static const uint8_t kTopRightInside[16] = {
    0, 0, 1, 0, 0, 0, 1, 0, 1, 1, 1, 0, 1, 0, 1, 0,
};

typedef struct {
    struct h264_dec *dec;
    bits_t *b;
    cabac_t *cab;
    rowbuf_t *row;
    mbinfo_t *cur;
    const mbinfo_t *left;
    const mbinfo_t *top;
    const mbinfo_t *topright;
    const mbinfo_t *topleft;
    uint32_t mb_x;
    uint32_t mb_y;
    uint8_t *py;
    uint8_t *pu;
    uint8_t *pv;
    uint8_t blk_has[24];
    uint8_t mvd[2][16][2];
    int qp;
} mbctx_t;

static bool read_ref(mbctx_t *c, int list, int x4, int y4, int w4, int h4, int count, int *out);
static bool read_cbp(mbctx_t *c, bool intra, int *out);
static bool read_qp_delta(mbctx_t *c);
static bool maybe_qp_delta(mbctx_t *c, bool present);
static int read_transform8x8(mbctx_t *c);
static int read_intra_mode(mbctx_t *c, int pred);
static uint32_t read_chroma_mode(mbctx_t *c);
static int read_mvd(mbctx_t *c, int list, int x4, int y4, int comp);
static void store_mvd(mbctx_t *c, int list, int x4, int y4, int w4, int h4, int mvdx, int mvdy);
static void finish_mvd(mbctx_t *c);
static void store_ref(mbctx_t *c, int list, int x4, int y4, int w4, int h4, int ref);
static int cabac_cbf_inc(const mbctx_t *c, int cat, int x4, int y4, int comp);

static inline bool parse_overrun(const mbctx_t *c) {
    return c->dec->cabac_on ? bits_past_buffer(c->b) : bits_overrun(c->b);
}

static inline bool is_inter(const mbinfo_t *m) {
    return m->kind == MB_INTER || m->kind == MB_SKIP;
}

static void setup_ctx(struct h264_dec *dec, mbctx_t *c, uint32_t mb_addr, uint16_t slice) {
    c->dec = dec;
    c->b = &dec->bits;
    c->cab = dec->cabac;
    c->mb_x = mb_addr % dec->mb_w;
    c->mb_y = mb_addr / dec->mb_w;
    c->row = &dec->rows[dec->cur_slot];
    const rowbuf_t *above = dec->above_slot >= 0 ? &dec->rows[dec->above_slot] : NULL;
    const uint32_t stride = dec->mb_stride;
    c->cur = mb_at(c->row, c->mb_x, stride);
    const mbinfo_t *left = c->mb_x > 0 ? mb_at(c->row, c->mb_x - 1, stride) : NULL;
    c->left = left && left->slice == slice ? left : NULL;
    c->top = NULL;
    c->topright = NULL;
    c->topleft = NULL;
    if (c->mb_y > 0 && above) {
        const mbinfo_t *t = mb_at(above, c->mb_x, stride);
        if (t->slice == slice) c->top = t;
        if (c->mb_x + 1 < dec->mb_w) {
            const mbinfo_t *tr = mb_at(above, c->mb_x + 1, stride);
            if (tr->slice == slice) c->topright = tr;
        }
        if (c->mb_x > 0) {
            const mbinfo_t *tl = mb_at(above, c->mb_x - 1, stride);
            if (tl->slice == slice) c->topleft = tl;
        }
    }
    c->py = c->row->y + c->mb_x * 16;
    c->pu = c->row->u + c->mb_x * 8;
    c->pv = c->row->v + c->mb_x * 8;
}

static void set_chroma_qp(struct h264_dec *dec, mbinfo_t *m, int qp) {
    const pps_t *pps = dec->cur_pps;
    m->cqp[0] = h264_chroma_qp[clip3(0, 51, qp + pps->chroma_qp_offset[0])];
    m->cqp[1] = h264_chroma_qp[clip3(0, 51, qp + pps->chroma_qp_offset[1])];
}

static int read_levels(struct h264_dec *dec, bits_t *b, int max_coeff, int nc, int *level,
                       int *zeros_out) {
    const vlc_t *vlc;
    if (nc < 0) vlc = &dec->coeff_token_dc;
    else if (nc < 2) vlc = &dec->coeff_token[0];
    else if (nc < 4) vlc = &dec->coeff_token[1];
    else if (nc < 8) vlc = &dec->coeff_token[2];
    else vlc = &dec->coeff_token[3];

    const int sym = vlc_read(b, vlc);
    if (sym < 0 || sym >= 4 * 17) return -1;
    const int total = sym >> 2;
    const int t1 = sym & 3;
    if (total == 0) return 0;
    if (total > max_coeff) return -1;

    int suffix_len = (total > 10 && t1 < 3) ? 1 : 0;
    for (int i = 0; i < total; i++) {
        if (i < t1) {
            level[i] = bits_u1(b) ? -1 : 1;
            continue;
        }
        const uint32_t v = bits_peek(b);
        const int prefix = clz32(v);
        if (prefix >= 32) return -1;
        int code;
        if (prefix < 14 && suffix_len == 0) {
            bits_skip(b, (unsigned)prefix + 1);
            code = prefix;
        } else if (prefix < 15 && suffix_len > 0) {
            code = (int)((prefix << suffix_len) + ((v << (prefix + 1)) >> (32 - suffix_len)));
            bits_skip(b, (unsigned)(prefix + 1 + suffix_len));
        } else {
            bits_skip(b, (unsigned)prefix + 1);
            code = (prefix < 15 ? prefix : 15) << suffix_len;
            int size;
            if (prefix == 14 && suffix_len == 0) size = 4;
            else if (prefix >= 15) size = prefix - 3;
            else size = suffix_len;
            if (size) code += (int)bits_u(b, (unsigned)size);
            if (prefix >= 15 && suffix_len == 0) code += 15;
            if (prefix >= 16) code += (1 << (prefix - 3)) - 4096;
        }
        if (i == t1 && t1 < 3) code += 2;
        const int lv = (code & 1) ? (-code - 1) >> 1 : (code + 2) >> 1;
        level[i] = lv;
        if (suffix_len == 0) suffix_len = 1;
        if (suffix_len < 6 && iabs(lv) > (3 << (suffix_len - 1))) suffix_len++;
    }

    int zeros = 0;
    if (total < max_coeff) {
        const vlc_t *tz = max_coeff == 4 ? &dec->total_zeros_dc[total - 1]
                                         : &dec->total_zeros[total - 1];
        zeros = vlc_read(b, tz);
        if (zeros < 0 || total + zeros > max_coeff) return -1;
    }
    *zeros_out = zeros;
    return total;
}

static int read_runs(struct h264_dec *dec, bits_t *b, int total, int zeros, uint8_t *pos) {
    int p = total + zeros - 1;
    for (int i = 0; i < total; i++) {
        pos[i] = (uint8_t)p;
        if (i == total - 1) break;
        if (zeros > 0) {
            const int run = vlc_read(b, &dec->run_before[(zeros < 7 ? zeros : 7) - 1]);
            if (run < 0 || run > zeros) return -1;
            zeros -= run;
            p -= run + 1;
        } else {
            p--;
        }
    }
    if (b->pos > (b->len + BITS_PADDING) * 8) return -1;
    return total;
}

static int read_block(struct h264_dec *dec, bits_t *b, int16_t *blk, int start, int max_coeff,
                      int nc, int qp, const int16_t *ls) {
    PROF_START(dec);
    int level[16];
    uint8_t pos[16];
    int zeros = 0;
    const int total = read_levels(dec, b, max_coeff, nc, level, &zeros);
    if (total <= 0) {
        PROF_STOP(dec, H264_PROF_CAVLC);
        return total;
    }
    if (read_runs(dec, b, total, zeros, pos) < 0) return -1;
    const int q6 = qp / 6;
    const int16_t *scale = ls + (qp % 6) * 16;
    if (q6 >= 4) {
        const int shift = q6 - 4;
        for (int i = 0; i < total; i++) {
            const int r = h264_zigzag4x4[start + pos[i]];
            blk[r] = (int16_t)((level[i] * scale[r]) << shift);
        }
    } else {
        const int shift = 4 - q6;
        const int round = 1 << (3 - q6);
        for (int i = 0; i < total; i++) {
            const int r = h264_zigzag4x4[start + pos[i]];
            blk[r] = (int16_t)((level[i] * scale[r] + round) >> shift);
        }
    }
    PROF_STOP(dec, H264_PROF_CAVLC);
    return total;
}

static int read_block8(struct h264_dec *dec, bits_t *b, int16_t *blk, int sub, int nc, int qp,
                       const int16_t *ls) {
    PROF_START(dec);
    int level[16];
    uint8_t pos[16];
    int zeros = 0;
    const int total = read_levels(dec, b, 16, nc, level, &zeros);
    if (total <= 0) {
        PROF_STOP(dec, H264_PROF_CAVLC);
        return total;
    }
    if (read_runs(dec, b, total, zeros, pos) < 0) return -1;
    const int q6 = qp / 6;
    const int16_t *scale = ls + (qp % 6) * 64;
    const int shift = q6 >= 6 ? q6 - 6 : 6 - q6;
    const int round = q6 >= 6 ? 0 : 1 << (5 - q6);
    for (int i = 0; i < total; i++) {
        const int r = h264_zigzag8x8[4 * pos[i] + sub];
        blk[r] = (int16_t)(q6 >= 6 ? (level[i] * scale[r]) << shift
                                   : (level[i] * scale[r] + round) >> shift);
    }
    PROF_STOP(dec, H264_PROF_CAVLC);
    return total;
}

static void place_levels(int16_t *blk, const int16_t *levels, const uint8_t *pos, int total,
                         int start, int qp, const int16_t *ls, bool wide) {
    const int q6 = qp / 6;
    const uint8_t *scan = wide ? h264_zigzag8x8 : h264_zigzag4x4;
    const int16_t *scale = ls + (qp % 6) * (wide ? 64 : 16);
    const int limit = wide ? 6 : 4;
    if (q6 >= limit) {
        const int shift = q6 - limit;
        for (int i = 0; i < total; i++) {
            const int r = scan[start + pos[i]];
            blk[r] = (int16_t)((levels[i] * scale[r]) << shift);
        }
        return;
    }
    const int shift = limit - q6;
    const int round = 1 << (limit - 1 - q6);
    for (int i = 0; i < total; i++) {
        const int r = scan[start + pos[i]];
        blk[r] = (int16_t)((levels[i] * scale[r] + round) >> shift);
    }
}

static int cabac_block(mbctx_t *c, int cat, int16_t *blk, int max_coeff, int start, int qp,
                       const int16_t *ls, int x4, int y4, int comp, const uint8_t *dc_order) {
    PROF_START(c->dec);
    if (cat != 5) {
        if (!h264_cabac_cbf(c->cab, cat, cabac_cbf_inc(c, cat, x4, y4, comp))) {
            PROF_STOP(c->dec, H264_PROF_CAVLC);
            return 0;
        }
    }
    int16_t levels[64];
    uint8_t pos[64];
    const int total = h264_cabac_residual(c->cab, cat, max_coeff, levels, pos);
    if (dc_order) {
        for (int i = 0; i < total; i++) blk[dc_order[pos[i]]] = levels[i];
    } else if (ls) {
        place_levels(blk, levels, pos, total, start, qp, ls, cat == 5);
    } else {
        for (int i = 0; i < total; i++) blk[pos[i]] = levels[i];
    }
    PROF_STOP(c->dec, H264_PROF_CAVLC);
    return total;
}

static int read_dc(struct h264_dec *dec, bits_t *b, int16_t *out, int max_coeff, int nc,
                   const uint8_t *order) {
    int level[16];
    uint8_t pos[16];
    int zeros = 0;
    const int total = read_levels(dec, b, max_coeff, nc, level, &zeros);
    if (total <= 0) return total;
    if (read_runs(dec, b, total, zeros, pos) < 0) return -1;
    for (int i = 0; i < total; i++) out[order ? order[pos[i]] : pos[i]] = (int16_t)level[i];
    return total;
}

static int nc_luma(const mbctx_t *c, int x4, int y4) {
    int na = -1, nb = -1;
    if (x4 > 0) na = c->cur->nnz[y4 * 4 + x4 - 1];
    else if (c->left) na = c->left->nnz[y4 * 4 + 3];
    if (y4 > 0) nb = c->cur->nnz[(y4 - 1) * 4 + x4];
    else if (c->top) nb = c->top->nnz[12 + x4];
    if (na >= 0 && nb >= 0) return (na + nb + 1) >> 1;
    if (na >= 0) return na;
    if (nb >= 0) return nb;
    return 0;
}

static int nc_chroma(const mbctx_t *c, int comp, int cx, int cy) {
    const int base = 16 + comp * 4;
    int na = -1, nb = -1;
    if (cx > 0) na = c->cur->nnz[base + cy * 2];
    else if (c->left) na = c->left->nnz[base + cy * 2 + 1];
    if (cy > 0) nb = c->cur->nnz[base + cx];
    else if (c->top) nb = c->top->nnz[base + 2 + cx];
    if (na >= 0 && nb >= 0) return (na + nb + 1) >> 1;
    if (na >= 0) return na;
    if (nb >= 0) return nb;
    return 0;
}

static bool residual(mbctx_t *c, bool i16, int cbp) {
    struct h264_dec *dec = c->dec;
    bits_t *b = c->b;
    mbinfo_t *m = c->cur;
    int16_t *coef = dec->coeff;
    const int cbp_luma = cbp & 15;
    const int cbp_chroma = cbp >> 4;
    const bool intra = c->cur->kind != MB_INTER && c->cur->kind != MB_SKIP;
    const int16_t *luma_ls = dec->ls4 + (intra ? 0 : 3) * 6 * 16;
    memset(c->blk_has, 0, sizeof(c->blk_has));

    if (m->t8x8) {
        const int16_t *ls8 = dec->ls8 + (intra ? 0 : 1) * 6 * 64;
        for (int blk8 = 0; blk8 < 4; blk8++) {
            int16_t *blk = coef + blk8 * 64;
            const int r0 = (blk8 >> 1) * 8 + (blk8 & 1) * 2;
            if (!(cbp_luma & (1 << blk8))) {
                m->nnz[r0] = m->nnz[r0 + 1] = m->nnz[r0 + 4] = m->nnz[r0 + 5] = 0;
                c->blk_has[blk8] = 0;
                continue;
            }
            memset(blk, 0, sizeof(int16_t) * 64);
            int any = 0;
            if (dec->cabac_on) {
                any = cabac_block(c, 5, blk, 64, 0, c->qp, ls8, 0, 0, 0, NULL);
                m->nnz[r0] = m->nnz[r0 + 1] = m->nnz[r0 + 4] = m->nnz[r0 + 5] = (uint8_t)any;
            } else {
                for (int sub = 0; sub < 4; sub++) {
                    const int x4 = (blk8 & 1) * 2 + (sub & 1);
                    const int y4 = (blk8 >> 1) * 2 + (sub >> 1);
                    const int t = read_block8(dec, b, blk, sub, nc_luma(c, x4, y4), c->qp, ls8);
                    if (t < 0) return false;
                    m->nnz[y4 * 4 + x4] = (uint8_t)t;
                    any |= t;
                }
            }
            c->blk_has[blk8] = any != 0;
        }
    } else if (i16) {
        memset(coef, 0, sizeof(int16_t) * 16 * 16);
        int16_t *dc = dec->dc;
        memset(dc, 0, sizeof(int16_t) * 16);
        const int n = dec->cabac_on
                          ? cabac_block(c, 0, dc, 16, 0, 0, NULL, 0, 0, 0, h264_zigzag4x4)
                          : read_dc(dec, b, dc, 16, nc_luma(c, 0, 0), h264_zigzag4x4);
        if (n < 0) return false;
        if (n > 0) m->cbf |= 1u << 8;
        if (n > 0) {
            h264_luma_dc_dequant(dc, c->qp, luma_ls[(c->qp % 6) * 16]);
            for (int r = 0; r < 16; r++) {
                coef[r * 16] = dc[r];
                c->blk_has[r] = dc[r] != 0;
            }
        }
        for (int z = 0; z < 16; z++) {
            const int x4 = kBlockX[z], y4 = kBlockY[z], r = y4 * 4 + x4;
            if (!cbp_luma) {
                m->nnz[r] = 0;
                continue;
            }
            const int t = dec->cabac_on
                              ? cabac_block(c, 1, coef + r * 16, 15, 1, c->qp, luma_ls, x4, y4, 0,
                                            NULL)
                              : read_block(dec, b, coef + r * 16, 1, 15, nc_luma(c, x4, y4), c->qp,
                                           luma_ls);
            if (t < 0) return false;
            m->nnz[r] = (uint8_t)t;
            if (t) c->blk_has[r] = 1;
        }
    } else {
        for (int z = 0; z < 16; z++) {
            const int x4 = kBlockX[z], y4 = kBlockY[z], r = y4 * 4 + x4;
            if (!(cbp_luma & (1 << (z >> 2)))) {
                m->nnz[r] = 0;
                continue;
            }
            int16_t *blk = coef + r * 16;
            memset(blk, 0, sizeof(int16_t) * 16);
            const int t = dec->cabac_on
                              ? cabac_block(c, 2, blk, 16, 0, c->qp, luma_ls, x4, y4, 0, NULL)
                              : read_block(dec, b, blk, 0, 16, nc_luma(c, x4, y4), c->qp, luma_ls);
            if (t < 0) return false;
            m->nnz[r] = (uint8_t)t;
            c->blk_has[r] = t != 0;
        }
    }

    if (!cbp_chroma) {
        memset(m->nnz + 16, 0, 8);
        return true;
    }
    int16_t dcc[2][4];
    for (int comp = 0; comp < 2; comp++) {
        memset(dcc[comp], 0, sizeof(dcc[comp]));
        const int n = dec->cabac_on
                          ? cabac_block(c, 3, dcc[comp], 4, 0, 0, NULL, 0, 0, comp, NULL)
                          : read_dc(dec, b, dcc[comp], 4, -1, NULL);
        if (n < 0) return false;
        if (n > 0) m->cbf |= 1u << (6 + comp);
        const int16_t *cls = dec->ls4 + ((intra ? 1 : 4) + comp) * 6 * 16;
        if (n) h264_chroma_dc_dequant(dcc[comp], m->cqp[comp], cls[(m->cqp[comp] % 6) * 16]);
    }
    for (int comp = 0; comp < 2; comp++) {
        const int base = 16 + comp * 4;
        const int cqp = m->cqp[comp];
        for (int k = 0; k < 4; k++) {
            int16_t *blk = coef + (base + k) * 16;
            memset(blk, 0, sizeof(int16_t) * 16);
            c->blk_has[base + k] = dcc[comp][k] != 0;
            if (cbp_chroma < 2) {
                m->nnz[base + k] = 0;
            } else {
                const int16_t *cls = dec->ls4 + ((intra ? 1 : 4) + comp) * 6 * 16;
                const int t = dec->cabac_on
                                  ? cabac_block(c, 4, blk, 15, 1, cqp, cls, k & 1, k >> 1, comp,
                                                NULL)
                                  : read_block(dec, b, blk, 1, 15,
                                               nc_chroma(c, comp, k & 1, k >> 1), cqp, cls);
                if (t < 0) return false;
                m->nnz[base + k] = (uint8_t)t;
                if (t) c->blk_has[base + k] = 1;
            }
            blk[0] = dcc[comp][k];
        }
    }
    return true;
}

static void add_chroma(mbctx_t *c) {
    PROF_START(c->dec);
    const uint32_t cs = c->dec->chroma_stride;
    int16_t *coef = c->dec->coeff;
    for (int comp = 0; comp < 2; comp++) {
        uint8_t *p = comp ? c->pv : c->pu;
        for (int k = 0; k < 4; k++) {
            const int idx = 16 + comp * 4 + k;
            if (!c->blk_has[idx]) continue;
            h264_idct4x4_add(p + (k >> 1) * 4 * cs + (k & 1) * 4, cs, coef + idx * 16);
        }
    }
    PROF_STOP(c->dec, H264_PROF_IDCT);
}

static void add_luma(mbctx_t *c) {
    PROF_START(c->dec);
    const uint32_t ls = c->dec->luma_stride;
    int16_t *coef = c->dec->coeff;
    if (c->cur->t8x8) {
        for (int blk8 = 0; blk8 < 4; blk8++) {
            if (!c->blk_has[blk8]) continue;
            h264_idct8x8_add(c->py + (blk8 >> 1) * 8 * ls + (blk8 & 1) * 8, ls, coef + blk8 * 64);
        }
        PROF_STOP(c->dec, H264_PROF_IDCT);
        return;
    }
    for (int r = 0; r < 16; r++) {
        if (!c->blk_has[r]) continue;
        h264_idct4x4_add(c->py + (r >> 2) * 4 * ls + (r & 3) * 4, ls, coef + r * 16);
    }
    PROF_STOP(c->dec, H264_PROF_IDCT);
}

static bool sample_avail(const mbctx_t *c, const mbinfo_t *m) {
    if (!m) return false;
    return !(c->dec->cur_pps->constrained_intra_pred && is_inter(m));
}

static unsigned mb_avail(const mbctx_t *c) {
    unsigned a = 0;
    if (sample_avail(c, c->left)) a |= AVAIL_LEFT;
    if (sample_avail(c, c->top)) a |= AVAIL_TOP;
    if (sample_avail(c, c->topleft)) a |= AVAIL_TOP_LEFT;
    if (sample_avail(c, c->topright)) a |= AVAIL_TOP_RIGHT;
    return a;
}

static uint16_t luma_nzmask(const mbinfo_t *m) {
    uint16_t mask = 0;
    if (m->t8x8) {
        for (int blk8 = 0; blk8 < 4; blk8++) {
            const int r0 = (blk8 >> 1) * 8 + (blk8 & 1) * 2;
            if (m->nnz[r0] || m->nnz[r0 + 1] || m->nnz[r0 + 4] || m->nnz[r0 + 5]) {
                mask |= (uint16_t)(0x33u << r0);
            }
        }
        return mask;
    }
    for (int r = 0; r < 16; r++) mask |= (uint16_t)((m->nnz[r] != 0) << r);
    return mask;
}

static void clear_inter(struct h264_dec *dec, mbinfo_t *m) {
    for (int list = 0; list < 2; list++) {
        if (list && !dec->has_l1) break;
        for (int i = 0; i < 4; i++) {
            m->m[list].ref[i] = -1;
            m->m[list].refpic[i] = NO_PIC;
        }
        memset(m->m[list].mv, 0, sizeof(m->m[list].mv));
    }
}

static bool intra_mb(mbctx_t *c, uint32_t type) {
    struct h264_dec *dec = c->dec;
    bits_t *b = c->b;
    mbinfo_t *m = c->cur;
    clear_inter(dec, m);
    const unsigned avail = mb_avail(c);
    const uint32_t ls = dec->luma_stride;
    const uint32_t cs = dec->chroma_stride;

    if (type == 25) {
        m->kind = MB_PCM;
        b->pos = (b->pos + 7) & ~7u;
        const uint8_t *p = b->buf + (b->pos >> 3);
        for (int y = 0; y < 16; y++) memcpy(c->py + y * ls, p + y * 16, 16);
        p += 256;
        for (int y = 0; y < 8; y++) memcpy(c->pu + y * cs, p + y * 8, 8);
        p += 64;
        for (int y = 0; y < 8; y++) memcpy(c->pv + y * cs, p + y * 8, 8);
        b->pos += 384 * 8;
        m->qp = 0;
        m->t8x8 = 0;
        m->cbf = 0x1EF;
        set_chroma_qp(dec, m, 0);
        memset(m->nnz, 16, sizeof(m->nnz));
        memset(m->modes, 2, sizeof(m->modes));
        if (dec->cabac_on) {
            h264_cabac_reinit(c->cab);
            return !bits_past_buffer(b);
        }
        return !bits_overrun(b);
    }

    int cbp;
    int i16_mode = 0;
    const bool i16 = type != 0;
    m->t8x8 = 0;
    if (i16) {
        if (type > 24) return false;
        m->kind = MB_INTRA16x16;
        i16_mode = (int)((type - 1) % 4);
        const int chroma = (int)((type - 1) / 4 % 3);
        cbp = (chroma << 4) | (type >= 13 ? 15 : 0);
        memset(m->modes, 2, sizeof(m->modes));
    } else {
        m->kind = MB_INTRA4x4;
        const bool constrained = dec->cur_pps->constrained_intra_pred;
        if (dec->cur_pps->transform_8x8_mode) m->t8x8 = (uint8_t)read_transform8x8(c);
        if (m->t8x8) {
            for (int blk8 = 0; blk8 < 4; blk8++) {
                const int bx = blk8 & 1, by = blk8 >> 1;
                const int x4 = bx * 2, y4 = by * 2;
                int ma = -1, mb = -1;
                bool dc = false;
                if (x4 > 0) {
                    ma = m->modes[y4 * 4 + x4 - 1];
                } else if (c->left) {
                    ma = c->left->modes[y4 * 4 + 3];
                    if (constrained && is_inter(c->left)) dc = true;
                } else {
                    dc = true;
                }
                if (y4 > 0) {
                    mb = m->modes[(y4 - 1) * 4 + x4];
                } else if (c->top) {
                    mb = c->top->modes[12 + x4];
                    if (constrained && is_inter(c->top)) dc = true;
                } else {
                    dc = true;
                }
                const int pred = dc ? 2 : imin(ma, mb);
                const int mode = read_intra_mode(c, pred);
                for (int k = 0; k < 4; k++) {
                    m->modes[(y4 + (k >> 1)) * 4 + x4 + (k & 1)] = (int8_t)mode;
                }
            }
        } else
        for (int z = 0; z < 16; z++) {
            const int x4 = kBlockX[z], y4 = kBlockY[z];
            int ma = -1, mb = -1;
            bool dc = false;
            if (x4 > 0) {
                ma = m->modes[y4 * 4 + x4 - 1];
            } else if (c->left) {
                ma = c->left->modes[y4 * 4 + 3];
                if (constrained && is_inter(c->left)) dc = true;
            } else {
                dc = true;
            }
            if (y4 > 0) {
                mb = m->modes[(y4 - 1) * 4 + x4];
            } else if (c->top) {
                mb = c->top->modes[12 + x4];
                if (constrained && is_inter(c->top)) dc = true;
            } else {
                dc = true;
            }
            const int pred = dc ? 2 : imin(ma, mb);
            m->modes[y4 * 4 + x4] = (int8_t)read_intra_mode(c, dc ? 2 : imin(ma, mb));
            (void)pred;
        }
    }
    const uint32_t chroma_mode = read_chroma_mode(c);
    if (chroma_mode > 3) return false;
    if (chroma_mode) m->cflags |= MB_CF_CHROMA;
    if (!i16) {
        if (!read_cbp(c, true, &cbp)) return false;
    }
    if (!maybe_qp_delta(c, cbp || i16)) return false;
    m->qp = (int8_t)c->qp;
    set_chroma_qp(dec, m, c->qp);
    m->cbf |= (uint16_t)(cbp & 0x3F);
    if (!residual(c, i16, cbp)) return false;

    {
        PROF_START(dec);
        h264_intra_chroma(c->pu, cs, (int)chroma_mode, avail);
        h264_intra_chroma(c->pv, cs, (int)chroma_mode, avail);
        PROF_STOP(dec, H264_PROF_INTRA);
    }
    add_chroma(c);

    if (i16) {
        {
            PROF_START(dec);
            h264_intra16x16(c->py, ls, i16_mode, avail);
            PROF_STOP(dec, H264_PROF_INTRA);
        }
        add_luma(c);
        return true;
    }
    int16_t *coef = dec->coeff;
    if (m->t8x8) {
        for (int blk8 = 0; blk8 < 4; blk8++) {
            const int bx = blk8 & 1, by = blk8 >> 1;
            unsigned a = 0;
            if (bx > 0 || (avail & AVAIL_LEFT)) a |= AVAIL_LEFT;
            if (by > 0 || (avail & AVAIL_TOP)) a |= AVAIL_TOP;
            if (bx > 0 && by > 0) a |= AVAIL_TOP_LEFT;
            else if (bx > 0) { if (avail & AVAIL_TOP) a |= AVAIL_TOP_LEFT; }
            else if (by > 0) { if (avail & AVAIL_LEFT) a |= AVAIL_TOP_LEFT; }
            else if (avail & AVAIL_TOP_LEFT) a |= AVAIL_TOP_LEFT;
            if (by > 0) {
                if (bx == 0) a |= AVAIL_TOP_RIGHT;
            } else if (bx == 0) {
                if (avail & AVAIL_TOP) a |= AVAIL_TOP_RIGHT;
            } else if (avail & AVAIL_TOP_RIGHT) {
                a |= AVAIL_TOP_RIGHT;
            }
            uint8_t *dst = c->py + by * 8 * ls + bx * 8;
            {
                PROF_START(dec);
                h264_intra8x8(dst, ls, m->modes[by * 8 + bx * 2], a);
                PROF_STOP(dec, H264_PROF_INTRA);
            }
            if (c->blk_has[blk8]) {
                PROF_START(dec);
                h264_idct8x8_add(dst, ls, coef + blk8 * 64);
                PROF_STOP(dec, H264_PROF_IDCT);
            }
        }
        return true;
    }
    for (int z = 0; z < 16; z++) {
        const int x4 = kBlockX[z], y4 = kBlockY[z], r = y4 * 4 + x4;
        unsigned a = 0;
        if (x4 > 0 || (avail & AVAIL_LEFT)) a |= AVAIL_LEFT;
        if (y4 > 0 || (avail & AVAIL_TOP)) a |= AVAIL_TOP;
        if (y4 > 0) {
            if (kTopRightInside[z]) a |= AVAIL_TOP_RIGHT;
        } else if (x4 < 3) {
            if (avail & AVAIL_TOP) a |= AVAIL_TOP_RIGHT;
        } else if (avail & AVAIL_TOP_RIGHT) {
            a |= AVAIL_TOP_RIGHT;
        }
        uint8_t *dst = c->py + y4 * 4 * ls + x4 * 4;
        {
            PROF_START(dec);
            h264_intra4x4(dst, ls, m->modes[r], a);
            PROF_STOP(dec, H264_PROF_INTRA);
        }
        if (c->blk_has[r]) {
            PROF_START(dec);
            h264_idct4x4_add(dst, ls, coef + r * 16);
            PROF_STOP(dec, H264_PROF_IDCT);
        }
    }
    return true;
}

typedef struct {
    bool avail;
    bool direct;
    int ref;
    int mvx;
    int mvy;
} nb_t;

static void get_nb(const mbctx_t *c, int list, int x4, int y4, int cur_z, nb_t *out) {
    const mbinfo_t *m;
    int bx, by;
    if (y4 < 0) {
        if (x4 < 0) {
            m = c->topleft;
            bx = 3;
        } else if (x4 >= 4) {
            m = c->topright;
            bx = x4 - 4;
        } else {
            m = c->top;
            bx = x4;
        }
        by = 3;
    } else if (x4 < 0) {
        m = c->left;
        bx = 3;
        by = y4;
    } else if (x4 >= 4 || kZIndex[y4][x4] >= cur_z) {
        m = NULL;
        bx = by = 0;
    } else {
        m = c->cur;
        bx = x4;
        by = y4;
    }
    out->direct = false;
    if (!m) {
        out->avail = false;
        out->ref = -1;
        out->mvx = out->mvy = 0;
    } else if (!is_inter(m)) {
        out->avail = true;
        out->ref = -1;
        out->mvx = out->mvy = 0;
    } else {
        const int blk8 = (by >> 1) * 2 + (bx >> 1);
        const mbmotion_t *mm = &m->m[list];
        out->avail = true;
        out->direct = (m->direct8 >> blk8) & 1;
        out->ref = mm->ref[blk8];
        out->mvx = mm->mv[by * 4 + bx][0];
        out->mvy = mm->mv[by * 4 + bx][1];
    }
}

static inline int median3(int a, int b, int c) {
    return a + b + c - imin(a, imin(b, c)) - imax(a, imax(b, c));
}

static void mv_pred(const mbctx_t *c, int list, int x4, int y4, int w4, int shape, int ref,
                    int *mvx, int *mvy) {
    const int cur_z = kZIndex[y4][x4];
    nb_t a, b, cc;
    get_nb(c, list, x4 - 1, y4, cur_z, &a);
    get_nb(c, list, x4, y4 - 1, cur_z, &b);
    get_nb(c, list, x4 + w4, y4 - 1, cur_z, &cc);
    if (!cc.avail) get_nb(c, list, x4 - 1, y4 - 1, cur_z, &cc);

    if (shape == SHAPE_16x8) {
        const nb_t *n = y4 == 0 ? &b : &a;
        if (n->ref == ref) {
            *mvx = n->mvx;
            *mvy = n->mvy;
            return;
        }
    } else if (shape == SHAPE_8x16) {
        const nb_t *n = x4 == 0 ? &a : &cc;
        if (n->ref == ref) {
            *mvx = n->mvx;
            *mvy = n->mvy;
            return;
        }
    }
    if (!b.avail && !cc.avail && a.avail) {
        b = a;
        cc = a;
    }
    const int matches = (a.ref == ref) + (b.ref == ref) + (cc.ref == ref);
    if (matches == 1) {
        const nb_t *n = a.ref == ref ? &a : b.ref == ref ? &b : &cc;
        *mvx = n->mvx;
        *mvy = n->mvy;
        return;
    }
    *mvx = median3(a.mvx, b.mvx, cc.mvx);
    *mvy = median3(a.mvy, b.mvy, cc.mvy);
}

static frame_t *ref_frame(struct h264_dec *dec, int list, int idx) {
    frame_t *f = idx < dec->ref_count[list] ? dec->ref_list[list][idx] : NULL;
    if (f && !f->non_existing) return f;
    dec->pic_error = true;
    if (dec->last_ref && !dec->last_ref->non_existing) return dec->last_ref;
    for (uint8_t i = 0; i < dec->ref_count[list]; i++) {
        frame_t *g = dec->ref_list[list][i];
        if (g && !g->non_existing) return g;
    }
    return NULL;
}

static void store_motion(mbctx_t *c, int list, int x4, int y4, int w4, int h4, int ref,
                         int mvx, int mvy, uint8_t pic) {
    mbmotion_t *mm = &c->cur->m[list];
    const int16_t vx = (int16_t)mvx, vy = (int16_t)mvy;
    for (int y = y4; y < y4 + h4; y++) {
        for (int x = x4; x < x4 + w4; x++) {
            mm->mv[y * 4 + x][0] = vx;
            mm->mv[y * 4 + x][1] = vy;
        }
    }
    for (int y = y4 >> 1; y <= (y4 + h4 - 1) >> 1; y++) {
        for (int x = x4 >> 1; x <= (x4 + w4 - 1) >> 1; x++) {
            mm->ref[y * 2 + x] = (int8_t)ref;
            mm->refpic[y * 2 + x] = pic;
        }
    }
}

static void mc_part(mbctx_t *c, const frame_t *f, int x4, int y4, int w4, int h4, int mvx, int mvy,
                    uint8_t *dy, ptrdiff_t ystride, uint8_t *du, uint8_t *dv, ptrdiff_t cstride) {
    struct h264_dec *dec = c->dec;
    const int px = (int)c->mb_x * 16 + x4 * 4;
    const int py = (int)c->mb_y * 16 + y4 * 4;
    if (!f) {
        for (int y = 0; y < h4 * 4; y++) memset(dy + y * ystride, 128, (size_t)w4 * 4);
        for (int y = 0; y < h4 * 2; y++) {
            memset(du + y * cstride, 128, (size_t)w4 * 2);
            memset(dv + y * cstride, 128, (size_t)w4 * 2);
        }
        return;
    }
    {
        PROF_START(dec);
        h264_mc_luma(dec, f, dy, ystride, px, py, w4 * 4, h4 * 4, mvx, mvy);
        PROF_STOP(dec, H264_PROF_MC_LUMA);
    }
    PROF_START(dec);
    h264_mc_chroma(dec, f, du, dv, cstride, px >> 1, py >> 1, w4 * 2, h4 * 2, mvx, mvy);
    PROF_STOP(dec, H264_PROF_MC_CHROMA);
}

static void weight_single(const slice_t *s, int list, int ref, uint8_t *dy, ptrdiff_t ystride,
                          uint8_t *du, uint8_t *dv, ptrdiff_t cstride, int h4) {
    if (!s->weighted || ref >= MAX_REFS) return;
    const weight_t *w = s->weights[list][ref];
    if (w[0].weight != (1 << s->luma_denom) || w[0].offset) {
        h264_weight_block(dy, ystride, h4 * 4, w[0].weight, w[0].offset, s->luma_denom);
    }
    const int16_t unit = (int16_t)(1 << s->chroma_denom);
    if (w[1].weight != unit || w[1].offset) {
        h264_weight_block(du, cstride, h4 * 2, w[1].weight, w[1].offset, s->chroma_denom);
    }
    if (w[2].weight != unit || w[2].offset) {
        h264_weight_block(dv, cstride, h4 * 2, w[2].weight, w[2].offset, s->chroma_denom);
    }
}

static void combine_bi(mbctx_t *c, const int ref[2], uint8_t *dy, ptrdiff_t ystride, uint8_t *du,
                       uint8_t *dv, ptrdiff_t cstride, int w4, int h4) {
    struct h264_dec *dec = c->dec;
    const slice_t *s = &dec->slice;
    const uint8_t idc = dec->cur_pps->weighted_bipred_idc;
    const int lw = w4 * 4, lh = h4 * 4, cw = w4 * 2, ch = h4 * 2;
    int w1 = 0, oy = 0, ou = 0, ov = 0, ldenom = 0, cdenom = 0;
    bool weighted = false;
    if (idc == 1 && s->weighted && ref[0] < MAX_REFS && ref[1] < MAX_REFS) {
        weighted = true;
        ldenom = s->luma_denom;
        cdenom = s->chroma_denom;
    } else if (idc == 2 && ref[0] < MAX_REFS && ref[1] < MAX_REFS) {
        w1 = dec->implicit_w[ref[0]][ref[1]];
        weighted = w1 != 32;
    }
    if (!weighted) {
        h264_avg_block(dy, ystride, dec->bi_y[0], dec->bi_y[1], 16, lh);
        h264_avg_block(du, cstride, dec->bi_u[0], dec->bi_u[1], 16, ch);
        h264_avg_block(dv, cstride, dec->bi_v[0], dec->bi_v[1], 16, ch);
        return;
    }
    if (idc == 1) {
        const weight_t *a = s->weights[0][ref[0]];
        const weight_t *b = s->weights[1][ref[1]];
        h264_weight_bi_block(dy, ystride, dec->bi_y[0], dec->bi_y[1], 16, lw, lh, a[0].weight,
                             b[0].weight, (a[0].offset + b[0].offset + 1) >> 1, ldenom);
        h264_weight_bi_block(du, cstride, dec->bi_u[0], dec->bi_u[1], 16, cw, ch, a[1].weight,
                             b[1].weight, (a[1].offset + b[1].offset + 1) >> 1, cdenom);
        h264_weight_bi_block(dv, cstride, dec->bi_v[0], dec->bi_v[1], 16, cw, ch, a[2].weight,
                             b[2].weight, (a[2].offset + b[2].offset + 1) >> 1, cdenom);
        return;
    }
    (void)oy;
    (void)ou;
    (void)ov;
    h264_weight_bi_implicit(dy, ystride, dec->bi_y[0], dec->bi_y[1], 16, lh, w1);
    h264_weight_bi_implicit(du, cstride, dec->bi_u[0], dec->bi_u[1], 16, ch, w1);
    h264_weight_bi_implicit(dv, cstride, dec->bi_v[0], dec->bi_v[1], 16, ch, w1);
}

static void predict_part(mbctx_t *c, int x4, int y4, int w4, int h4, int pred, const int ref[2],
                         const int mv[2][2]) {
    struct h264_dec *dec = c->dec;
    const uint32_t ls = dec->luma_stride;
    const uint32_t cs = dec->chroma_stride;
    uint8_t *dy = c->py + y4 * 4 * ls + x4 * 4;
    uint8_t *du = c->pu + y4 * 2 * cs + x4 * 2;
    uint8_t *dv = c->pv + y4 * 2 * cs + x4 * 2;
    const frame_t *frames[2] = { NULL, NULL };

    for (int list = 0; list < 2; list++) {
        if (list && !dec->has_l1) break;
        if (!(pred & (1 << list))) {
            store_motion(c, list, x4, y4, w4, h4, -1, 0, 0, NO_PIC);
            continue;
        }
        const frame_t *f = ref_frame(dec, list, ref[list]);
        frames[list] = f;
        store_motion(c, list, x4, y4, w4, h4, ref[list], mv[list][0], mv[list][1],
                     f ? (uint8_t)(f - dec->frames) : NO_PIC);
    }

    if (pred == 3) {
        for (int list = 0; list < 2; list++) {
            mc_part(c, frames[list], x4, y4, w4, h4, mv[list][0], mv[list][1], dec->bi_y[list], 16,
                    dec->bi_u[list], dec->bi_v[list], 16);
        }
        combine_bi(c, ref, dy, ls, du, dv, cs, w4, h4);
        return;
    }
    const int list = pred == 2 ? 1 : 0;
    mc_part(c, frames[list], x4, y4, w4, h4, mv[list][0], mv[list][1], dy, ls, du, dv, cs);
    if (frames[list]) weight_single(&dec->slice, list, ref[list], dy, ls, du, dv, cs, h4);
}

static void predict_l0(mbctx_t *c, int x4, int y4, int w4, int h4, int ref, int mvx, int mvy) {
    const int refs[2] = { ref, 0 };
    const int mv[2][2] = { { mvx, mvy }, { 0, 0 } };
    predict_part(c, x4, y4, w4, h4, 1, refs, mv);
}

static inline int min_positive(int a, int b) {
    return (a >= 0 && b >= 0) ? imin(a, b) : imax(a, b);
}

static const colblk_t *colocated_blocks(struct h264_dec *dec, uint32_t mb_x, uint32_t mb_y) {
    const frame_t *col = dec->ref_count[1] ? dec->ref_list[1][0] : NULL;
    if (!col || !col->col) return NULL;
    return (const colblk_t *)col->col + ((size_t)mb_y * dec->mb_w + mb_x) * dec->col_blocks;
}

static int map_col_ref(struct h264_dec *dec, uint8_t pic) {
    if (pic != NO_PIC) {
        for (uint8_t i = 0; i < dec->ref_count[0]; i++) {
            const frame_t *f = dec->ref_list[0][i];
            if (f && (uint8_t)(f - dec->frames) == pic) return i;
        }
    }
    dec->pic_error = true;
    return 0;
}

static void direct_blocks(mbctx_t *c, int blk8_only, int *x4, int *y4, int *size, int *first,
                          int *count) {
    const bool wide = c->dec->col_blocks == 4;
    *size = wide ? 2 : 1;
    (void)x4;
    (void)y4;
    if (blk8_only >= 0) {
        *first = wide ? blk8_only : blk8_only * 4;
        *count = wide ? 1 : 4;
    } else {
        *first = 0;
        *count = wide ? 4 : 16;
    }
}

static void direct_block_pos(const struct h264_dec *dec, int idx, int blk8_only, int *x4, int *y4) {
    if (dec->col_blocks == 4) {
        *x4 = (idx & 1) * 2;
        *y4 = (idx >> 1) * 2;
        return;
    }
    int r = idx;
    if (blk8_only >= 0) {
        const int base = (blk8_only >> 1) * 8 + (blk8_only & 1) * 2;
        const int n = idx - blk8_only * 4;
        r = base + (n & 1) + (n >> 1) * 4;
    }
    *x4 = r & 3;
    *y4 = r >> 2;
}

static int direct_col_index(const struct h264_dec *dec, int x4, int y4) {
    if (dec->col_blocks == 4) return (y4 >> 1) * 2 + (x4 >> 1);
    return y4 * 4 + x4;
}

static void direct_spatial_mb(mbctx_t *c, int blk8_only) {
    struct h264_dec *dec = c->dec;
    int ref[2];
    int mv[2][2] = { { 0, 0 }, { 0, 0 } };
    for (int list = 0; list < 2; list++) {
        nb_t a, b, cc;
        get_nb(c, list, -1, 0, 0, &a);
        get_nb(c, list, 0, -1, 0, &b);
        get_nb(c, list, 4, -1, 0, &cc);
        if (!cc.avail) get_nb(c, list, -1, -1, 0, &cc);
        ref[list] = min_positive(a.ref, min_positive(b.ref, cc.ref));
    }
    bool zero = false;
    if (ref[0] < 0 && ref[1] < 0) {
        ref[0] = ref[1] = 0;
        zero = true;
    }
    const int pred = (ref[0] >= 0 ? 1 : 0) | (ref[1] >= 0 ? 2 : 0);
    const int refs[2] = { ref[0] >= 0 ? ref[0] : 0, ref[1] >= 0 ? ref[1] : 0 };
    if (!zero) {
        for (int list = 0; list < 2; list++) {
            if (ref[list] >= 0) {
                mv_pred(c, list, 0, 0, 4, SHAPE_16x16, ref[list], &mv[list][0], &mv[list][1]);
            }
        }
    }

    const colblk_t *col = colocated_blocks(dec, c->mb_x, c->mb_y);
    const frame_t *colpic = dec->ref_count[1] ? dec->ref_list[1][0] : NULL;
    const bool col_short = colpic && colpic->ref == REF_SHORT && col != NULL;
    int dummy_x = 0, dummy_y = 0, size = 0, first = 0, count = 0;
    direct_blocks(c, blk8_only, &dummy_x, &dummy_y, &size, &first, &count);
    if (blk8_only < 0 && count > 1) {
        bool same = true;
        bool zero0 = false;
        for (int n = 0; n < count && same; n++) {
            int x4, y4;
            direct_block_pos(dec, first + n, blk8_only, &x4, &y4);
            bool zero = false;
            if (col_short) {
                const colblk_t *cb = &col[direct_col_index(dec, x4, y4)];
                zero = cb->ref == 0 && iabs(cb->mv[0]) <= 1 && iabs(cb->mv[1]) <= 1;
            }
            if (n == 0) zero0 = zero;
            else same = zero == zero0;
        }
        if (same) {
            int bmv[2][2];
            for (int list = 0; list < 2; list++) {
                const bool zero_mv = zero0 && ref[list] == 0;
                bmv[list][0] = zero_mv ? 0 : mv[list][0];
                bmv[list][1] = zero_mv ? 0 : mv[list][1];
            }
            c->cur->uniform = 1;
            predict_part(c, 0, 0, 4, 4, pred, refs, bmv);
            return;
        }
    }
    for (int n = 0; n < count; n++) {
        int x4, y4;
        direct_block_pos(dec, first + n, blk8_only, &x4, &y4);
        bool col_zero = false;
        if (col_short) {
            const colblk_t *cb = &col[direct_col_index(dec, x4, y4)];
            col_zero = cb->ref == 0 && iabs(cb->mv[0]) <= 1 && iabs(cb->mv[1]) <= 1;
        }
        int bmv[2][2];
        for (int list = 0; list < 2; list++) {
            const bool zero_mv = col_zero && ref[list] == 0;
            bmv[list][0] = zero_mv ? 0 : mv[list][0];
            bmv[list][1] = zero_mv ? 0 : mv[list][1];
        }
        predict_part(c, x4, y4, size, size, pred, refs, bmv);
    }
}

static void direct_temporal_mb(mbctx_t *c, int blk8_only) {
    struct h264_dec *dec = c->dec;
    const colblk_t *col = colocated_blocks(dec, c->mb_x, c->mb_y);
    const frame_t *colpic = dec->ref_count[1] ? dec->ref_list[1][0] : NULL;
    int dummy_x = 0, dummy_y = 0, size = 0, first = 0, count = 0;
    direct_blocks(c, blk8_only, &dummy_x, &dummy_y, &size, &first, &count);
    if (blk8_only < 0 && count > 1 && col) {
        const colblk_t *first_cb = &col[direct_col_index(dec, 0, 0)];
        bool same = true;
        for (int n = 1; n < count && same; n++) {
            int x4, y4;
            direct_block_pos(dec, first + n, blk8_only, &x4, &y4);
            const colblk_t *cb = &col[direct_col_index(dec, x4, y4)];
            same = cb->ref == first_cb->ref && cb->pic == first_cb->pic &&
                   cb->mv[0] == first_cb->mv[0] && cb->mv[1] == first_cb->mv[1];
        }
        if (same) count = 1;
    }
    const int merged = count == 1 && blk8_only < 0 ? 4 : size;
    for (int n = 0; n < count; n++) {
        int x4, y4;
        direct_block_pos(dec, first + n, blk8_only, &x4, &y4);
        int mvcol[2] = { 0, 0 };
        int ref0 = 0;
        if (col) {
            const colblk_t *cb = &col[direct_col_index(dec, x4, y4)];
            if (cb->ref >= 0) {
                mvcol[0] = cb->mv[0];
                mvcol[1] = cb->mv[1];
                ref0 = map_col_ref(dec, cb->pic);
            }
        }
        const frame_t *f0 = ref0 < dec->ref_count[0] ? dec->ref_list[0][ref0] : NULL;
        int mv[2][2];
        int td = 0;
        if (f0 && colpic) td = clip3(-128, 127, colpic->poc - f0->poc);
        if (!f0 || f0->ref == REF_LONG || td == 0) {
            mv[0][0] = mvcol[0];
            mv[0][1] = mvcol[1];
            mv[1][0] = 0;
            mv[1][1] = 0;
        } else {
            const int tb = clip3(-128, 127, dec->cur_poc - f0->poc);
            const int tx = (16384 + iabs(td / 2)) / td;
            const int dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6);
            for (int i = 0; i < 2; i++) {
                mv[0][i] = (dsf * mvcol[i] + 128) >> 8;
                mv[1][i] = mv[0][i] - mvcol[i];
            }
        }
        const int refs[2] = { ref0, 0 };
        if (merged == 4) c->cur->uniform = 1;
        predict_part(c, x4, y4, merged, merged, 3, refs, mv);
    }
}

static void direct_mb(mbctx_t *c, int blk8_only) {
    if (c->dec->slice.direct_spatial) direct_spatial_mb(c, blk8_only);
    else direct_temporal_mb(c, blk8_only);
}

static inline bool mb_is_intra(const mbinfo_t *m) {
    return m->kind != MB_INTER && m->kind != MB_SKIP;
}

static bool test_not_skip(const mbinfo_t *m) { return m->kind != MB_SKIP; }
static bool test_i16_pcm(const mbinfo_t *m) {
    return m->kind == MB_INTRA16x16 || m->kind == MB_PCM;
}
static bool test_not_direct(const mbinfo_t *m) { return !(m->cflags & MB_CF_DIRECT); }
static bool test_chroma_mode(const mbinfo_t *m) { return (m->cflags & MB_CF_CHROMA) != 0; }
static bool test_t8x8(const mbinfo_t *m) { return m->t8x8 != 0; }

static int ctx_sum(const mbctx_t *c, bool (*test)(const mbinfo_t *)) {
    int inc = 0;
    if (c->left && test(c->left)) inc++;
    if (c->top && test(c->top)) inc++;
    return inc;
}

static int cabac_cbf_inc(const mbctx_t *c, int cat, int x4, int y4, int comp) {
    const bool intra = mb_is_intra(c->cur);
    int nza, nzb;
    if (cat == 0 || cat == 3) {
        const int bit = cat == 0 ? 8 : 6 + comp;
        nza = c->left ? ((c->left->cbf >> bit) & 1) : (intra ? 1 : 0);
        nzb = c->top ? ((c->top->cbf >> bit) & 1) : (intra ? 1 : 0);
        if (c->left && c->left->kind == MB_PCM) nza = 1;
        if (c->top && c->top->kind == MB_PCM) nzb = 1;
        return nza + 2 * nzb;
    }
    const int base = cat >= 3 ? 16 + comp * 4 : 0;
    if (x4 > 0) {
        nza = cat >= 3 ? c->cur->nnz[base + (y4 << 1) + x4 - 1] : c->cur->nnz[y4 * 4 + x4 - 1];
    } else if (c->left) {
        nza = cat >= 3 ? c->left->nnz[base + (y4 << 1) + 1] : c->left->nnz[y4 * 4 + 3];
    } else {
        nza = intra ? 1 : 0;
    }
    if (y4 > 0) {
        nzb = cat >= 3 ? c->cur->nnz[base + x4] : c->cur->nnz[(y4 - 1) * 4 + x4];
    } else if (c->top) {
        nzb = cat >= 3 ? c->top->nnz[base + 2 + x4] : c->top->nnz[12 + x4];
    } else {
        nzb = intra ? 1 : 0;
    }
    return (nza > 0) + 2 * (nzb > 0);
}

static int mvd_neighbour(const mbctx_t *c, int list, int x4, int y4, int comp) {
    int a, b;
    if (x4 > 0) a = c->mvd[list][y4 * 4 + x4 - 1][comp];
    else if (c->left) a = c->left->m[list].mvd_right[y4][comp];
    else a = 0;
    if (y4 > 0) b = c->mvd[list][(y4 - 1) * 4 + x4][comp];
    else if (c->top) b = c->top->m[list].mvd_bottom[x4][comp];
    else b = 0;
    return a + b;
}

static void store_mvd(mbctx_t *c, int list, int x4, int y4, int w4, int h4, int mvdx, int mvdy) {
    const uint8_t ax = (uint8_t)imin(iabs(mvdx), 127);
    const uint8_t ay = (uint8_t)imin(iabs(mvdy), 127);
    for (int y = y4; y < y4 + h4; y++) {
        for (int x = x4; x < x4 + w4; x++) {
            c->mvd[list][y * 4 + x][0] = ax;
            c->mvd[list][y * 4 + x][1] = ay;
        }
    }
}

static void finish_mvd(mbctx_t *c) {
    struct h264_dec *dec = c->dec;
    for (int list = 0; list < 2; list++) {
        if (list && !dec->has_l1) break;
        mbmotion_t *mm = &c->cur->m[list];
        for (int i = 0; i < 4; i++) {
            mm->mvd_right[i][0] = c->mvd[list][i * 4 + 3][0];
            mm->mvd_right[i][1] = c->mvd[list][i * 4 + 3][1];
            mm->mvd_bottom[i][0] = c->mvd[list][12 + i][0];
            mm->mvd_bottom[i][1] = c->mvd[list][12 + i][1];
        }
    }
}

static int read_mvd(mbctx_t *c, int list, int x4, int y4, int comp) {
    if (!c->dec->cabac_on) return bits_se(c->b);
    const int amvd = mvd_neighbour(c, list, x4, y4, comp);
    return h264_cabac_mvd(c->cab, comp ? 47 : 40, amvd);
}

static void store_ref(mbctx_t *c, int list, int x4, int y4, int w4, int h4, int ref) {
    mbmotion_t *mm = &c->cur->m[list];
    for (int y = y4 >> 1; y <= (y4 + h4 - 1) >> 1; y++) {
        for (int x = x4 >> 1; x <= (x4 + w4 - 1) >> 1; x++) mm->ref[y * 2 + x] = (int8_t)ref;
    }
}

static int cabac_ref_inc(const mbctx_t *c, int list, int x4, int y4) {
    const bool b_slice = c->dec->slice.type == SLICE_B;
    nb_t a, b;
    get_nb(c, list, x4 - 1, y4, kZIndex[y4][x4], &a);
    get_nb(c, list, x4, y4 - 1, kZIndex[y4][x4], &b);
    int inc = 0;
    if (a.ref > 0 && !(b_slice && a.direct)) inc++;
    if (b.ref > 0 && !(b_slice && b.direct)) inc += 2;
    return inc;
}

static bool read_ref(mbctx_t *c, int list, int x4, int y4, int w4, int h4, int count, int *out) {
    if (count <= 1) {
        *out = 0;
        if (c->dec->cabac_on) store_ref(c, list, x4, y4, w4, h4, 0);
        return true;
    }
    uint32_t v;
    if (c->dec->cabac_on) {
        v = (uint32_t)h264_cabac_ref(c->cab, cabac_ref_inc(c, list, x4, y4));
    } else {
        v = count == 2 ? !bits_u1(c->b) : bits_ue(c->b);
    }
    if (v >= (uint32_t)count) return false;
    *out = (int)v;
    if (c->dec->cabac_on) store_ref(c, list, x4, y4, w4, h4, (int)v);
    return true;
}

static bool read_cbp(mbctx_t *c, bool intra, int *out) {
    if (c->dec->cabac_on) {
        const int left = c->left ? (c->left->cbf & 0x3F) : 0x0F;
        const int top = c->top ? (c->top->cbf & 0x3F) : 0x0F;
        int cbp = h264_cabac_cbp_luma(c->cab, left & 15, top & 15);
        cbp |= h264_cabac_cbp_chroma(c->cab, (left >> 4) & 3, (top >> 4) & 3) << 4;
        *out = cbp;
        return true;
    }
    const uint32_t code = bits_ue(c->b);
    if (code > 47) return false;
    *out = intra ? kCbpIntra[code] : kCbpInter[code];
    return true;
}

static bool read_qp_delta(mbctx_t *c) {
    int32_t dq;
    if (c->dec->cabac_on) {
        dq = h264_cabac_qp_delta(c->cab, c->dec->last_qp_delta_nonzero);
        c->dec->last_qp_delta_nonzero = dq != 0;
    } else {
        dq = bits_se(c->b);
    }
    if (dq < -26 || dq > 25) return false;
    c->qp = (c->qp + dq + 52) % 52;
    return true;
}

static bool maybe_qp_delta(mbctx_t *c, bool present) {
    if (present) return read_qp_delta(c);
    c->dec->last_qp_delta_nonzero = false;
    return true;
}

static int read_transform8x8(mbctx_t *c) {
    if (!c->dec->cabac_on) return (int)bits_u1(c->b);
    return h264_cabac_transform8x8(c->cab, ctx_sum(c, test_t8x8));
}

static int read_intra_mode(mbctx_t *c, int pred) {
    if (c->dec->cabac_on) return h264_cabac_intra_pred_mode(c->cab, pred);
    if (bits_u1(c->b)) return pred;
    const int rem = (int)bits_u(c->b, 3);
    return rem < pred ? rem : rem + 1;
}

static uint32_t read_chroma_mode(mbctx_t *c) {
    if (c->dec->cabac_on) return (uint32_t)h264_cabac_chroma_mode(c->cab, ctx_sum(c, test_chroma_mode));
    return bits_ue(c->b);
}

static bool inter_mb(mbctx_t *c, uint32_t type) {
    struct h264_dec *dec = c->dec;
    bits_t *b = c->b;
    mbinfo_t *m = c->cur;
    m->kind = MB_INTER;
    m->t8x8 = 0;
    memset(m->modes, 2, sizeof(m->modes));
    for (int i = 0; i < 4; i++) m->m[0].ref[i] = -1;
    if (dec->has_l1) {
        for (int i = 0; i < 4; i++) m->m[1].ref[i] = -1;
    }
    const int refs = dec->slice.num_ref_idx_active;
    int ref[4] = { 0, 0, 0, 0 };
    int mvd[16][2];
    bool small_parts = false;

    if (type < 3) {
        const int parts = type == 0 ? 1 : 2;
        int px[2] = { 0, 0 }, py[2] = { 0, 0 }, pw[2] = { 4, 4 }, ph[2] = { 4, 4 };
        int shape = SHAPE_16x16;
        for (int p = 0; p < parts; p++) {
            if (type == 1) {
                py[p] = p * 2;
                ph[p] = 2;
                shape = SHAPE_16x8;
            } else if (type == 2) {
                px[p] = p * 2;
                pw[p] = 2;
                shape = SHAPE_8x16;
            }
        }
        for (int p = 0; p < parts; p++) {
            if (!read_ref(c, 0, px[p], py[p], pw[p], ph[p], refs, &ref[p])) return false;
        }
        for (int p = 0; p < parts; p++) {
            for (int comp = 0; comp < 2; comp++) {
                mvd[p][comp] = read_mvd(c, 0, px[p], py[p], comp);
            }
            store_mvd(c, 0, px[p], py[p], pw[p], ph[p], mvd[p][0], mvd[p][1]);
            int mvx, mvy;
            mv_pred(c, 0, px[p], py[p], pw[p], shape, ref[p], &mvx, &mvy);
            predict_l0(c, px[p], py[p], pw[p], ph[p], ref[p], mvx + mvd[p][0], mvy + mvd[p][1]);
        }
    } else {
        uint32_t sub[4];
        for (int i = 0; i < 4; i++) {
            sub[i] = dec->cabac_on ? (uint32_t)h264_cabac_sub_type_p(c->cab) : bits_ue(b);
            if (sub[i] > 3) return false;
        }
        static const uint8_t kSubParts[4] = { 1, 2, 2, 4 };
        for (int i = 0; i < 4; i++) small_parts |= sub[i] != 0;
        if (type == 3) {
            for (int i = 0; i < 4; i++) {
                if (!read_ref(c, 0, (i & 1) * 2, (i >> 1) * 2, 2, 2, refs, &ref[i])) return false;
            }
        }
        if (dec->cabac_on) {
            for (int i = 0; i < 4; i++) {
                const int bx = (i & 1) * 2, by = (i >> 1) * 2;
                for (int k = 0; k < kSubParts[sub[i]]; k++) {
                    int x4 = bx, y4 = by, w4 = 2, h4 = 2;
                    switch (sub[i]) {
                    case 1: y4 += k; h4 = 1; break;
                    case 2: x4 += k; w4 = 1; break;
                    case 3: x4 += k & 1; y4 += k >> 1; w4 = h4 = 1; break;
                    default: break;
                    }
                    const int dx = read_mvd(c, 0, x4, y4, 0);
                    const int dy = read_mvd(c, 0, x4, y4, 1);
                    store_mvd(c, 0, x4, y4, w4, h4, dx, dy);
                    int mvx, mvy;
                    mv_pred(c, 0, x4, y4, w4, SHAPE_SUB, ref[i], &mvx, &mvy);
                    predict_l0(c, x4, y4, w4, h4, ref[i], mvx + dx, mvy + dy);
                }
            }
        } else {
            int n = 0;
            for (int i = 0; i < 4; i++) {
                for (int k = 0; k < kSubParts[sub[i]]; k++, n++) {
                    mvd[n][0] = bits_se(b);
                    mvd[n][1] = bits_se(b);
                }
            }
            n = 0;
            for (int i = 0; i < 4; i++) {
                const int bx = (i & 1) * 2, by = (i >> 1) * 2;
                for (int k = 0; k < kSubParts[sub[i]]; k++, n++) {
                    int x4 = bx, y4 = by, w4 = 2, h4 = 2;
                    switch (sub[i]) {
                    case 1: y4 += k; h4 = 1; break;
                    case 2: x4 += k; w4 = 1; break;
                    case 3: x4 += k & 1; y4 += k >> 1; w4 = h4 = 1; break;
                    default: break;
                    }
                    int mvx, mvy;
                    mv_pred(c, 0, x4, y4, w4, SHAPE_SUB, ref[i], &mvx, &mvy);
                    predict_l0(c, x4, y4, w4, h4, ref[i], mvx + mvd[n][0], mvy + mvd[n][1]);
                }
            }
        }
    }
    if (parse_overrun(c)) return false;

    int cbp;
    if (!read_cbp(c, false, &cbp)) return false;
    if ((cbp & 15) && dec->cur_pps->transform_8x8_mode && !small_parts) {
        m->t8x8 = (uint8_t)read_transform8x8(c);
    }
    if (!maybe_qp_delta(c, cbp != 0)) return false;
    m->qp = (int8_t)c->qp;
    set_chroma_qp(dec, m, c->qp);
    m->uniform = type == 0;
    m->nzmask = 0;
    m->cbf |= (uint16_t)(cbp & 0x3F);
    finish_mvd(c);
    if (!cbp) {
        memset(m->nnz, 0, sizeof(m->nnz));
        return true;
    }
    if (!residual(c, false, cbp)) return false;
    m->nzmask = luma_nzmask(m);
    add_luma(c);
    add_chroma(c);
    return true;
}

typedef struct {
    uint8_t shape;
    uint8_t pred[2];
} btype_t;

static const btype_t kBTypes[22] = {
    { SHAPE_16x16, { 0, 0 } },
    { SHAPE_16x16, { 1, 0 } }, { SHAPE_16x16, { 2, 0 } }, { SHAPE_16x16, { 3, 0 } },
    { SHAPE_16x8, { 1, 1 } },  { SHAPE_8x16, { 1, 1 } },
    { SHAPE_16x8, { 2, 2 } },  { SHAPE_8x16, { 2, 2 } },
    { SHAPE_16x8, { 1, 2 } },  { SHAPE_8x16, { 1, 2 } },
    { SHAPE_16x8, { 2, 1 } },  { SHAPE_8x16, { 2, 1 } },
    { SHAPE_16x8, { 1, 3 } },  { SHAPE_8x16, { 1, 3 } },
    { SHAPE_16x8, { 2, 3 } },  { SHAPE_8x16, { 2, 3 } },
    { SHAPE_16x8, { 3, 1 } },  { SHAPE_8x16, { 3, 1 } },
    { SHAPE_16x8, { 3, 2 } },  { SHAPE_8x16, { 3, 2 } },
    { SHAPE_16x8, { 3, 3 } },  { SHAPE_8x16, { 3, 3 } },
};

static const uint8_t kBSubPred[13] = { 0, 1, 2, 3, 1, 1, 2, 2, 3, 3, 1, 2, 3 };
static const uint8_t kBSubW[13] = { 2, 2, 2, 2, 2, 1, 2, 1, 2, 1, 1, 1, 1 };
static const uint8_t kBSubH[13] = { 2, 2, 2, 2, 1, 2, 1, 2, 1, 2, 1, 1, 1 };
static const uint8_t kBSubCount[13] = { 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 4, 4, 4 };

static void b_direct_setup(mbctx_t *c) {
    mbinfo_t *m = c->cur;
    m->kind = MB_INTER;
    m->t8x8 = 0;
    m->uniform = 0;
    m->cflags |= MB_CF_DIRECT;
    m->direct8 = 0x0F;
    memset(m->modes, 2, sizeof(m->modes));
}

static bool b_mb(mbctx_t *c, uint32_t type) {
    struct h264_dec *dec = c->dec;
    bits_t *b = c->b;
    mbinfo_t *m = c->cur;
    const slice_t *s = &dec->slice;
    m->kind = MB_INTER;
    m->t8x8 = 0;
    memset(m->modes, 2, sizeof(m->modes));
    for (int i = 0; i < 4; i++) {
        m->m[0].ref[i] = -1;
        m->m[1].ref[i] = -1;
    }
    const int counts[2] = { s->num_ref_idx_active, s->num_ref_idx_l1 };
    bool small_parts = false;
    bool uniform = false;

    if (type == 0) {
        b_direct_setup(c);
        direct_mb(c, -1);
        small_parts = !dec->active.direct_8x8_inference;
    } else if (type <= 21) {
        const btype_t *bt = &kBTypes[type];
        const int parts = bt->shape == SHAPE_16x16 ? 1 : 2;
        int ref[2][2] = { { 0, 0 }, { 0, 0 } };
        int mvd[2][2][2] = { { { 0, 0 }, { 0, 0 } }, { { 0, 0 }, { 0, 0 } } };
        int px[2] = { 0, 0 }, py[2] = { 0, 0 }, pw[2] = { 4, 4 }, ph[2] = { 4, 4 };
        for (int p = 0; p < parts; p++) {
            if (bt->shape == SHAPE_16x8) {
                py[p] = p * 2;
                ph[p] = 2;
            } else if (bt->shape == SHAPE_8x16) {
                px[p] = p * 2;
                pw[p] = 2;
            }
        }
        for (int list = 0; list < 2; list++) {
            for (int p = 0; p < parts; p++) {
                if (bt->pred[p] & (1 << list)) {
                    if (!read_ref(c, list, px[p], py[p], pw[p], ph[p], counts[list],
                                  &ref[list][p])) {
                        return false;
                    }
                }
            }
        }
        for (int list = 0; list < 2; list++) {
            for (int p = 0; p < parts; p++) {
                if (bt->pred[p] & (1 << list)) {
                    mvd[list][p][0] = read_mvd(c, list, px[p], py[p], 0);
                    mvd[list][p][1] = read_mvd(c, list, px[p], py[p], 1);
                    store_mvd(c, list, px[p], py[p], pw[p], ph[p], mvd[list][p][0],
                              mvd[list][p][1]);
                }
            }
        }
        for (int p = 0; p < parts; p++) {
            int mv[2][2] = { { 0, 0 }, { 0, 0 } };
            int refs[2] = { 0, 0 };
            for (int list = 0; list < 2; list++) {
                if (!(bt->pred[p] & (1 << list))) continue;
                int mx, my;
                mv_pred(c, list, px[p], py[p], pw[p], bt->shape, ref[list][p], &mx, &my);
                mv[list][0] = mx + mvd[list][p][0];
                mv[list][1] = my + mvd[list][p][1];
                refs[list] = ref[list][p];
            }
            predict_part(c, px[p], py[p], pw[p], ph[p], bt->pred[p], refs, mv);
        }
        uniform = bt->shape == SHAPE_16x16;
    } else {
        uint32_t sub[4];
        for (int i = 0; i < 4; i++) {
            sub[i] = dec->cabac_on ? (uint32_t)h264_cabac_sub_type_b(c->cab) : bits_ue(b);
            if (sub[i] > 12) return false;
            if (sub[i] == 0) {
                small_parts |= !dec->active.direct_8x8_inference;
                m->direct8 |= (uint8_t)(1 << i);
            } else {
                small_parts |= kBSubW[sub[i]] * kBSubH[sub[i]] != 4;
            }
        }
        int ref[2][4] = { { 0, 0, 0, 0 }, { 0, 0, 0, 0 } };
        for (int list = 0; list < 2; list++) {
            for (int i = 0; i < 4; i++) {
                if (sub[i] && (kBSubPred[sub[i]] & (1 << list))) {
                    if (!read_ref(c, list, (i & 1) * 2, (i >> 1) * 2, 2, 2, counts[list],
                                  &ref[list][i])) {
                        return false;
                    }
                }
            }
        }
        int mvd[2][16][2];
        memset(mvd, 0, sizeof(mvd));
        for (int list = 0; list < 2; list++) {
            for (int i = 0; i < 4; i++) {
                if (!sub[i] || !(kBSubPred[sub[i]] & (1 << list))) continue;
                const int bx = (i & 1) * 2, by = (i >> 1) * 2;
                const int w4 = kBSubW[sub[i]], h4 = kBSubH[sub[i]];
                for (int k = 0; k < kBSubCount[sub[i]]; k++) {
                    int x4 = bx, y4 = by;
                    if (w4 == 2 && h4 == 1) y4 += k;
                    else if (w4 == 1 && h4 == 2) x4 += k;
                    else if (w4 == 1 && h4 == 1) {
                        x4 += k & 1;
                        y4 += k >> 1;
                    }
                    mvd[list][i * 4 + k][0] = read_mvd(c, list, x4, y4, 0);
                    mvd[list][i * 4 + k][1] = read_mvd(c, list, x4, y4, 1);
                    store_mvd(c, list, x4, y4, w4, h4, mvd[list][i * 4 + k][0],
                              mvd[list][i * 4 + k][1]);
                }
            }
        }
        for (int i = 0; i < 4; i++) {
            const int bx = (i & 1) * 2, by = (i >> 1) * 2;
            if (!sub[i]) {
                direct_mb(c, i);
                continue;
            }
            const int pred = kBSubPred[sub[i]];
            const int w4 = kBSubW[sub[i]], h4 = kBSubH[sub[i]];
            for (int k = 0; k < kBSubCount[sub[i]]; k++) {
                int px4 = bx, py4 = by;
                if (w4 == 2 && h4 == 1) py4 += k;
                else if (w4 == 1 && h4 == 2) px4 += k;
                else if (w4 == 1 && h4 == 1) {
                    px4 += k & 1;
                    py4 += k >> 1;
                }
                int mv[2][2] = { { 0, 0 }, { 0, 0 } };
                int refs[2] = { 0, 0 };
                for (int list = 0; list < 2; list++) {
                    if (!(pred & (1 << list))) continue;
                    int mx, my;
                    mv_pred(c, list, px4, py4, w4, SHAPE_SUB, ref[list][i], &mx, &my);
                    mv[list][0] = mx + mvd[list][i * 4 + k][0];
                    mv[list][1] = my + mvd[list][i * 4 + k][1];
                    refs[list] = ref[list][i];
                }
                predict_part(c, px4, py4, w4, h4, pred, refs, mv);
            }
        }
    }
    if (parse_overrun(c)) return false;

    int cbp;
    if (!read_cbp(c, false, &cbp)) return false;
    if ((cbp & 15) && dec->cur_pps->transform_8x8_mode && !small_parts) {
        m->t8x8 = (uint8_t)read_transform8x8(c);
    }
    if (!maybe_qp_delta(c, cbp != 0)) return false;
    m->qp = (int8_t)c->qp;
    set_chroma_qp(dec, m, c->qp);
    m->uniform = uniform ? 1 : 0;
    m->nzmask = 0;
    m->cbf |= (uint16_t)(cbp & 0x3F);
    finish_mvd(c);
    if (!cbp) {
        memset(m->nnz, 0, sizeof(m->nnz));
        return true;
    }
    if (!residual(c, false, cbp)) return false;
    m->nzmask = luma_nzmask(m);
    add_luma(c);
    add_chroma(c);
    return true;
}

static void b_skip_mb(struct h264_dec *dec, mbctx_t *c) {
    mbinfo_t *m = c->cur;
    b_direct_setup(c);
    m->kind = MB_SKIP;
    m->nzmask = 0;
    m->qp = (int8_t)c->qp;
    set_chroma_qp(dec, m, c->qp);
    memset(m->nnz, 0, sizeof(m->nnz));
    direct_mb(c, -1);
}

static void begin_mb(struct h264_dec *dec, mbctx_t *c, uint32_t mb_addr) {
    const slice_t *s = &dec->slice;
    if (s->type != SLICE_I && (mb_addr % dec->mb_w == 0 || mb_addr == s->first_mb)) {
        h264_window_advance(dec, mb_addr / dec->mb_w);
    }
    setup_ctx(dec, c, mb_addr, dec->slice_num);
    mbinfo_t *m = c->cur;
    m->slice = dec->slice_num;
    m->filter = s->disable_deblock;
    m->alpha = s->alpha_offset;
    m->beta = s->beta_offset;
    m->cflags = 0;
    m->direct8 = 0;
    m->cbf = 0;
    if (dec->cabac_on) {
        memset(c->mvd, 0, sizeof(c->mvd));
        finish_mvd(c);
    }
}

static void skip_mb(struct h264_dec *dec, mbctx_t *c) {
    mbinfo_t *m = c->cur;
    m->kind = MB_SKIP;
    m->t8x8 = 0;
    m->uniform = 1;
    m->nzmask = 0;
    m->qp = (int8_t)c->qp;
    set_chroma_qp(dec, m, c->qp);
    memset(m->nnz, 0, sizeof(m->nnz));
    memset(m->modes, 2, sizeof(m->modes));
    for (int i = 0; i < 4; i++) m->m[0].ref[i] = -1;

    nb_t a, b;
    get_nb(c, 0, -1, 0, 0, &a);
    get_nb(c, 0, 0, -1, 0, &b);
    int mvx = 0, mvy = 0;
    if (a.avail && b.avail && !(a.ref == 0 && a.mvx == 0 && a.mvy == 0) &&
        !(b.ref == 0 && b.mvx == 0 && b.mvy == 0)) {
        mv_pred(c, 0, 0, 0, 4, SHAPE_16x16, 0, &mvx, &mvy);
    }
    predict_l0(c, 0, 0, 4, 4, 0, mvx, mvy);
}

static void end_mb(struct h264_dec *dec, const mbctx_t *c) {
    if (c->mb_x == (uint32_t)dec->mb_w - 1) h264_row_done(dec, c->mb_y);
}

static bool decode_slice_cabac(struct h264_dec *dec) {
    bits_t *b = &dec->bits;
    const slice_t *s = &dec->slice;
    cabac_t *cab = dec->cabac;
    const uint32_t total = (uint32_t)dec->mb_w * dec->mb_h;
    uint32_t mb = s->first_mb;
    int qp = s->qp;
    bool ok = true;
    mbctx_t c;

    b->pos = (b->pos + 7) & ~7u;
    bits_reserve(b, BITS_MB_RESERVE);
    h264_cabac_init(cab, b, s->qp, s->type == SLICE_I ? -1 : s->cabac_init_idc);
    dec->last_qp_delta_nonzero = false;

    for (;;) {
        bits_reserve(b, BITS_MB_RESERVE);
        begin_mb(dec, &c, mb);
        c.qp = qp;
        bool skip = false;
        if (s->type != SLICE_I) {
            const int inc = ctx_sum(&c, test_not_skip);
            skip = h264_cabac_decision(cab, (s->type == SLICE_B ? 24 : 11) + inc) != 0;
        }
        if (skip) {
            if (s->type == SLICE_B) b_skip_mb(dec, &c);
            else skip_mb(dec, &c);
            dec->last_qp_delta_nonzero = false;
        } else {
            int type;
            if (s->type == SLICE_I) {
                type = h264_cabac_mb_type_i(cab, 3, ctx_sum(&c, test_i16_pcm), true);
            } else if (s->type == SLICE_P) {
                type = h264_cabac_mb_type_p(cab);
            } else {
                type = h264_cabac_mb_type_b(cab, ctx_sum(&c, test_not_direct));
            }
            bool mb_ok;
            if (s->type == SLICE_P && type < 5) {
                mb_ok = inter_mb(&c, (uint32_t)type);
            } else if (s->type == SLICE_B && type < 23) {
                mb_ok = b_mb(&c, (uint32_t)type);
            } else {
                if (s->type == SLICE_P) type -= 5;
                else if (s->type == SLICE_B) type -= 23;
                mb_ok = type >= 0 && type <= 25 && intra_mb(&c, (uint32_t)type);
            }
            if (!mb_ok || bits_past_buffer(b)) {
                ok = false;
                break;
            }
        }
        qp = c.qp;
        end_mb(dec, &c);
        mb++;
        if (h264_cabac_terminate(cab)) break;
        if (mb >= total || bits_past_buffer(b)) {
            ok = false;
            break;
        }
    }
    dec->next_mb = mb;
    return ok;
}

bool h264_decode_slice_data(struct h264_dec *dec) {
    if (dec->cabac_on) return decode_slice_cabac(dec);
    bits_t *b = &dec->bits;
    const slice_t *s = &dec->slice;
    const uint32_t total = (uint32_t)dec->mb_w * dec->mb_h;
    uint32_t mb = s->first_mb;
    int qp = s->qp;
    bool ok = true;
    mbctx_t c;

    for (;;) {
        bits_reserve(b, BITS_MB_RESERVE);
        bool more = true;
        if (s->type != SLICE_I) {
            const uint32_t run = bits_ue(b);
            if (bits_overrun(b) || run > total - mb) {
                ok = false;
                break;
            }
            for (uint32_t i = 0; i < run; i++) {
                begin_mb(dec, &c, mb);
                c.qp = qp;
                if (s->type == SLICE_B) b_skip_mb(dec, &c);
                else skip_mb(dec, &c);
                end_mb(dec, &c);
                mb++;
            }
            if (run > 0) more = bits_more_data(b);
            if (!more) break;
            if (mb >= total) {
                ok = false;
                break;
            }
        }

        begin_mb(dec, &c, mb);
        c.qp = qp;
        uint32_t type = bits_ue(b);
        bool mb_ok;
        if (s->type == SLICE_P && type < 5) {
            mb_ok = inter_mb(&c, type);
        } else if (s->type == SLICE_B && type < 23) {
            mb_ok = b_mb(&c, type);
        } else {
            if (s->type == SLICE_P) type -= 5;
            else if (s->type == SLICE_B) type -= 23;
            mb_ok = type <= 25 && intra_mb(&c, type);
        }
        if (!mb_ok || bits_overrun(b)) {
            ok = false;
            break;
        }
        qp = c.qp;
        end_mb(dec, &c);
        mb++;
        if (!bits_more_data(b)) break;
        if (mb >= total) {
            ok = false;
            break;
        }
    }
    dec->next_mb = mb;
    return ok;
}

void h264_conceal_mb(struct h264_dec *dec, uint32_t mb_addr) {
    mbctx_t c;
    setup_ctx(dec, &c, mb_addr, 0xFFFF);
    mbinfo_t *m = c.cur;
    m->slice = 0xFFFF;
    m->kind = MB_SKIP;
    m->t8x8 = 0;
    m->uniform = 1;
    m->nzmask = 0;
    m->filter = 1;
    m->alpha = 0;
    m->beta = 0;
    m->qp = 0;
    set_chroma_qp(dec, m, 0);
    memset(m->nnz, 0, sizeof(m->nnz));
    memset(m->modes, 2, sizeof(m->modes));
    predict_l0(&c, 0, 0, 4, 4, 0, 0, 0);
    end_mb(dec, &c);
}
