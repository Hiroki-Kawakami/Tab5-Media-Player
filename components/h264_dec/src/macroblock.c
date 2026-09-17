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
    int qp;
} mbctx_t;

static inline bool is_inter(const mbinfo_t *m) {
    return m->kind == MB_INTER || m->kind == MB_SKIP;
}

static void setup_ctx(struct h264_dec *dec, mbctx_t *c, uint32_t mb_addr, uint16_t slice) {
    c->dec = dec;
    c->b = &dec->bits;
    c->mb_x = mb_addr % dec->mb_w;
    c->mb_y = mb_addr / dec->mb_w;
    c->row = &dec->rows[dec->cur_slot];
    const rowbuf_t *above = dec->above_slot >= 0 ? &dec->rows[dec->above_slot] : NULL;
    c->cur = &c->row->mb[c->mb_x];
    c->left = c->mb_x > 0 && c->row->mb[c->mb_x - 1].slice == slice ? &c->row->mb[c->mb_x - 1] : NULL;
    c->top = NULL;
    c->topright = NULL;
    c->topleft = NULL;
    if (c->mb_y > 0 && above) {
        const mbinfo_t *t = &above->mb[c->mb_x];
        if (t->slice == slice) c->top = t;
        if (c->mb_x + 1 < dec->mb_w && above->mb[c->mb_x + 1].slice == slice) {
            c->topright = &above->mb[c->mb_x + 1];
        }
        if (c->mb_x > 0 && above->mb[c->mb_x - 1].slice == slice) {
            c->topleft = &above->mb[c->mb_x - 1];
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
                      int nc, int qp) {
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
    const uint8_t *dq = h264_dequant4[qp % 6];
    const int scale = 1 << (qp / 6);
    for (int i = 0; i < total; i++) {
        const int r = h264_zigzag4x4[start + pos[i]];
        blk[r] = (int16_t)(level[i] * dq[r] * scale);
    }
    PROF_STOP(dec, H264_PROF_CAVLC);
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
    memset(c->blk_has, 0, sizeof(c->blk_has));

    if (i16) {
        memset(coef, 0, sizeof(int16_t) * 16 * 16);
        int16_t *dc = dec->dc;
        memset(dc, 0, sizeof(int16_t) * 16);
        const int n = read_dc(dec, b, dc, 16, nc_luma(c, 0, 0), h264_zigzag4x4);
        if (n < 0) return false;
        if (n > 0) {
            h264_luma_dc_dequant(dc, c->qp);
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
            const int t = read_block(dec, b, coef + r * 16, 1, 15, nc_luma(c, x4, y4), c->qp);
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
            const int t = read_block(dec, b, blk, 0, 16, nc_luma(c, x4, y4), c->qp);
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
        const int n = read_dc(dec, b, dcc[comp], 4, -1, NULL);
        if (n < 0) return false;
        if (n) h264_chroma_dc_dequant(dcc[comp], m->cqp[comp]);
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
                const int t = read_block(dec, b, blk, 1, 15, nc_chroma(c, comp, k & 1, k >> 1), cqp);
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

static void clear_inter(mbinfo_t *m) {
    for (int i = 0; i < 4; i++) {
        m->ref[i] = -1;
        m->refpic[i] = NO_PIC;
    }
    memset(m->mv, 0, sizeof(m->mv));
}

static bool intra_mb(mbctx_t *c, uint32_t type) {
    struct h264_dec *dec = c->dec;
    bits_t *b = c->b;
    mbinfo_t *m = c->cur;
    clear_inter(m);
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
        set_chroma_qp(dec, m, 0);
        memset(m->nnz, 16, sizeof(m->nnz));
        memset(m->modes, 2, sizeof(m->modes));
        return !bits_overrun(b);
    }

    int cbp;
    int i16_mode = 0;
    const bool i16 = type != 0;
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
            int mode;
            if (bits_u1(b)) {
                mode = pred;
            } else {
                const int rem = (int)bits_u(b, 3);
                mode = rem < pred ? rem : rem + 1;
            }
            m->modes[y4 * 4 + x4] = (int8_t)mode;
        }
    }
    const uint32_t chroma_mode = bits_ue(b);
    if (chroma_mode > 3) return false;
    if (!i16) {
        const uint32_t code = bits_ue(b);
        if (code > 47) return false;
        cbp = kCbpIntra[code];
    }
    if (cbp || i16) {
        const int32_t dq = bits_se(b);
        if (dq < -26 || dq > 25) return false;
        c->qp = (c->qp + dq + 52) % 52;
    }
    m->qp = (int8_t)c->qp;
    set_chroma_qp(dec, m, c->qp);
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
    int ref;
    int mvx;
    int mvy;
} nb_t;

static void get_nb(const mbctx_t *c, int x4, int y4, int cur_z, nb_t *out) {
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
    if (!m) {
        out->avail = false;
        out->ref = -1;
        out->mvx = out->mvy = 0;
    } else if (!is_inter(m)) {
        out->avail = true;
        out->ref = -1;
        out->mvx = out->mvy = 0;
    } else {
        out->avail = true;
        out->ref = m->ref[(by >> 1) * 2 + (bx >> 1)];
        out->mvx = m->mv[by * 4 + bx][0];
        out->mvy = m->mv[by * 4 + bx][1];
    }
}

static inline int median3(int a, int b, int c) {
    return a + b + c - imin(a, imin(b, c)) - imax(a, imax(b, c));
}

static void mv_pred(const mbctx_t *c, int x4, int y4, int w4, int shape, int ref, int *mvx,
                    int *mvy) {
    const int cur_z = kZIndex[y4][x4];
    nb_t a, b, cc;
    get_nb(c, x4 - 1, y4, cur_z, &a);
    get_nb(c, x4, y4 - 1, cur_z, &b);
    get_nb(c, x4 + w4, y4 - 1, cur_z, &cc);
    if (!cc.avail) get_nb(c, x4 - 1, y4 - 1, cur_z, &cc);

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

static frame_t *ref_frame(struct h264_dec *dec, int idx) {
    frame_t *f = idx < dec->ref_count ? dec->ref_list[idx] : NULL;
    if (f && !f->non_existing) return f;
    dec->pic_error = true;
    if (dec->last_ref && !dec->last_ref->non_existing) return dec->last_ref;
    for (uint8_t i = 0; i < dec->ref_count; i++) {
        if (dec->ref_list[i] && !dec->ref_list[i]->non_existing) return dec->ref_list[i];
    }
    return NULL;
}

static void predict_part(mbctx_t *c, int x4, int y4, int w4, int h4, int ref, int mvx, int mvy) {
    struct h264_dec *dec = c->dec;
    mbinfo_t *m = c->cur;
    const int16_t vx = (int16_t)mvx, vy = (int16_t)mvy;
    for (int y = y4; y < y4 + h4; y++) {
        for (int x = x4; x < x4 + w4; x++) {
            m->mv[y * 4 + x][0] = vx;
            m->mv[y * 4 + x][1] = vy;
        }
    }
    const frame_t *f = ref_frame(dec, ref);
    const uint8_t pic = f ? (uint8_t)(f - dec->frames) : NO_PIC;
    for (int y = y4 >> 1; y <= (y4 + h4 - 1) >> 1; y++) {
        for (int x = x4 >> 1; x <= (x4 + w4 - 1) >> 1; x++) {
            m->ref[y * 2 + x] = (int8_t)ref;
            m->refpic[y * 2 + x] = pic;
        }
    }
    const uint32_t ls = dec->luma_stride;
    const uint32_t cs = dec->chroma_stride;
    const int px = (int)c->mb_x * 16 + x4 * 4;
    const int py = (int)c->mb_y * 16 + y4 * 4;
    uint8_t *dy = c->py + y4 * 4 * ls + x4 * 4;
    uint8_t *du = c->pu + y4 * 2 * cs + x4 * 2;
    uint8_t *dv = c->pv + y4 * 2 * cs + x4 * 2;
    if (!f) {
        for (int y = 0; y < h4 * 4; y++) memset(dy + y * ls, 128, (size_t)w4 * 4);
        for (int y = 0; y < h4 * 2; y++) {
            memset(du + y * cs, 128, (size_t)w4 * 2);
            memset(dv + y * cs, 128, (size_t)w4 * 2);
        }
        return;
    }
    {
        PROF_START(dec);
        h264_mc_luma(dec, f, dy, ls, px, py, w4 * 4, h4 * 4, vx, vy);
        PROF_STOP(dec, H264_PROF_MC_LUMA);
    }
    PROF_START(dec);
    h264_mc_chroma(dec, f, du, dv, cs, px >> 1, py >> 1, w4 * 2, h4 * 2, vx, vy);
    PROF_STOP(dec, H264_PROF_MC_CHROMA);
}

static bool read_ref(bits_t *b, int count, int *out) {
    if (count <= 1) {
        *out = 0;
        return true;
    }
    const uint32_t v = count == 2 ? !bits_u1(b) : bits_ue(b);
    if (v >= (uint32_t)count) return false;
    *out = (int)v;
    return true;
}

static bool inter_mb(mbctx_t *c, uint32_t type) {
    struct h264_dec *dec = c->dec;
    bits_t *b = c->b;
    mbinfo_t *m = c->cur;
    m->kind = MB_INTER;
    memset(m->modes, 2, sizeof(m->modes));
    for (int i = 0; i < 4; i++) m->ref[i] = -1;
    const int refs = dec->slice.num_ref_idx_active;
    int ref[4] = { 0, 0, 0, 0 };
    int mvd[16][2];

    if (type < 3) {
        const int parts = type == 0 ? 1 : 2;
        for (int p = 0; p < parts; p++) {
            if (!read_ref(b, refs, &ref[p])) return false;
        }
        for (int p = 0; p < parts; p++) {
            mvd[p][0] = bits_se(b);
            mvd[p][1] = bits_se(b);
        }
        for (int p = 0; p < parts; p++) {
            int x4 = 0, y4 = 0, w4 = 4, h4 = 4, shape = SHAPE_16x16;
            if (type == 1) {
                y4 = p * 2;
                h4 = 2;
                shape = SHAPE_16x8;
            } else if (type == 2) {
                x4 = p * 2;
                w4 = 2;
                shape = SHAPE_8x16;
            }
            int mvx, mvy;
            mv_pred(c, x4, y4, w4, shape, ref[p], &mvx, &mvy);
            predict_part(c, x4, y4, w4, h4, ref[p], mvx + mvd[p][0], mvy + mvd[p][1]);
        }
    } else {
        uint32_t sub[4];
        for (int i = 0; i < 4; i++) {
            sub[i] = bits_ue(b);
            if (sub[i] > 3) return false;
        }
        if (type == 3) {
            for (int i = 0; i < 4; i++) {
                if (!read_ref(b, refs, &ref[i])) return false;
            }
        }
        static const uint8_t kSubParts[4] = { 1, 2, 2, 4 };
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
                mv_pred(c, x4, y4, w4, SHAPE_SUB, ref[i], &mvx, &mvy);
                predict_part(c, x4, y4, w4, h4, ref[i], mvx + mvd[n][0], mvy + mvd[n][1]);
            }
        }
    }
    if (bits_overrun(b)) return false;

    const uint32_t code = bits_ue(b);
    if (code > 47) return false;
    const int cbp = kCbpInter[code];
    if (cbp) {
        const int32_t dq = bits_se(b);
        if (dq < -26 || dq > 25) return false;
        c->qp = (c->qp + dq + 52) % 52;
    }
    m->qp = (int8_t)c->qp;
    set_chroma_qp(dec, m, c->qp);
    m->uniform = type == 0;
    m->nzmask = 0;
    if (!cbp) {
        memset(m->nnz, 0, sizeof(m->nnz));
        return true;
    }
    if (!residual(c, false, cbp)) return false;
    uint16_t mask = 0;
    for (int r = 0; r < 16; r++) mask |= (uint16_t)((m->nnz[r] != 0) << r);
    m->nzmask = mask;
    add_luma(c);
    add_chroma(c);
    return true;
}

static void begin_mb(struct h264_dec *dec, mbctx_t *c, uint32_t mb_addr) {
    const slice_t *s = &dec->slice;
    if (s->type == SLICE_P && (mb_addr % dec->mb_w == 0 || mb_addr == s->first_mb)) {
        h264_window_advance(dec, mb_addr / dec->mb_w);
    }
    setup_ctx(dec, c, mb_addr, dec->slice_num);
    mbinfo_t *m = c->cur;
    m->slice = dec->slice_num;
    m->filter = s->disable_deblock;
    m->alpha = s->alpha_offset;
    m->beta = s->beta_offset;
}

static void skip_mb(struct h264_dec *dec, mbctx_t *c) {
    mbinfo_t *m = c->cur;
    m->kind = MB_SKIP;
    m->uniform = 1;
    m->nzmask = 0;
    m->qp = (int8_t)c->qp;
    set_chroma_qp(dec, m, c->qp);
    memset(m->nnz, 0, sizeof(m->nnz));
    memset(m->modes, 2, sizeof(m->modes));
    for (int i = 0; i < 4; i++) m->ref[i] = -1;

    nb_t a, b;
    get_nb(c, -1, 0, 0, &a);
    get_nb(c, 0, -1, 0, &b);
    int mvx = 0, mvy = 0;
    if (a.avail && b.avail && !(a.ref == 0 && a.mvx == 0 && a.mvy == 0) &&
        !(b.ref == 0 && b.mvx == 0 && b.mvy == 0)) {
        mv_pred(c, 0, 0, 4, SHAPE_16x16, 0, &mvx, &mvy);
    }
    predict_part(c, 0, 0, 4, 4, 0, mvx, mvy);
}

static void end_mb(struct h264_dec *dec, const mbctx_t *c) {
    if (c->mb_x == (uint32_t)dec->mb_w - 1) h264_row_done(dec, c->mb_y);
}

bool h264_decode_slice_data(struct h264_dec *dec) {
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
                skip_mb(dec, &c);
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
        } else {
            if (s->type == SLICE_P) type -= 5;
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
    m->uniform = 1;
    m->nzmask = 0;
    m->filter = 1;
    m->alpha = 0;
    m->beta = 0;
    m->qp = 0;
    set_chroma_qp(dec, m, 0);
    memset(m->nnz, 0, sizeof(m->nnz));
    memset(m->modes, 2, sizeof(m->modes));
    predict_part(&c, 0, 0, 4, 4, 0, 0, 0);
    end_mb(dec, &c);
}
