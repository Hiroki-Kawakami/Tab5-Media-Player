/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "mpeg2_internal.h"
#include "vdec_kernels.h"

static inline int quantiser_scale(const struct mpeg2_dec *dec, uint32_t code) {
    return dec->pic.q_scale_type ? mpeg2_nonlinear_qscale[code] : (int)code * 2;
}

static void reset_dc(worker_t *w) {
    const int v = 1 << (7 + w->dec->pic.dc_precision);
    w->dc_pred[0] = w->dc_pred[1] = w->dc_pred[2] = v;
}

static bool read_motion(worker_t *w, int s, int mv[2]) {
    const struct mpeg2_dec *dec = w->dec;
    bits_t *b = &w->bits;
    for (int t = 0; t < 2; t++) {
        const int code = vlc_get(b, &dec->vlc_mv);
        if (code == VLC_ERROR) return false;
        const int r_size = dec->pic.f_code[s][t] - 1;
        int delta = code;
        if (r_size > 0 && code != 0) {
            const int residual = (int)bits_u(b, (unsigned)r_size);
            delta = ((code < 0 ? -code : code) - 1) * (1 << r_size) + residual + 1;
            if (code < 0) delta = -delta;
        }
        const int f = 1 << r_size;
        int v = w->pmv[0][s][t] + delta;
        if (v < -16 * f) v += 32 * f;
        else if (v > 16 * f - 1) v -= 32 * f;
        w->pmv[0][s][t] = w->pmv[1][s][t] = v;
        mv[t] = v;
    }
    return true;
}

static inline uint32_t peek_at(const uint8_t *buf, uint32_t pos) {
    const uint8_t *p = buf + (pos >> 3);
    const unsigned s = pos & 7;
    const uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    return (v << s) | (uint32_t)(p[4] >> (8 - s));
}

enum { DCT_ROOT = 10 };

typedef struct {
    int mismatch;
    uint8_t rows;
    uint8_t rows_ac;
} block_state_t;

static bool read_coefficients(worker_t *w, const vlc_t *vlc, int n, bool first_short,
                              const uint8_t *q, int nonintra, block_state_t *state) {
    bits_t *b = &w->bits;
    const uint8_t *buf = b->buf;
    const uint32_t *table = vlc->table;
    uint32_t pos = b->pos;
    int16_t *blk = w->blk;
    const uint8_t *scan = w->dec->pic.alternate_scan ? mpeg2_alternate : mpeg2_zigzag;
    const int qs = w->qscale;
    int mismatch = state->mismatch;
    uint32_t rows = state->rows, rows_ac = state->rows_ac;
    bool ok = false;
    for (;;) {
        int run, level;
        const uint32_t v = peek_at(buf, pos);
        if (first_short && (int32_t)v < 0) {
            pos += 2;
            run = 0;
            level = (v & 0x40000000u) ? -1 : 1;
        } else {
            uint32_t e = table[v >> (32 - DCT_ROOT)];
            if (e & VLC_SUB) {
                const unsigned sub = (e >> 1) & 31;
                e = table[(e >> 8) + ((v << DCT_ROOT) >> (32 - sub))];
                pos += DCT_ROOT;
            }
            if (!(e & 0x3E)) break;
            pos += (e >> 1) & 31;
            const int value = (int32_t)e >> 8;
            if (value == DCT_EOB) {
                ok = pos <= b->len * 8;
                break;
            }
            if (value == DCT_ESCAPE) {
                const uint32_t x = peek_at(buf, pos);
                pos += 18;
                run = (int)(x >> 26);
                level = (int32_t)(x << 6) >> 20;
                if (level == 0 || level == -2048) break;
            } else {
                run = value & 31;
                level = value >> 5;
            }
        }
        first_short = false;
        n += run;
        if (n > 63) break;
        const int j = scan[n++];
        const int mag = level < 0 ? -level : level;
        int c = ((2 * mag + nonintra) * q[j] * qs) >> 5;
        if (level < 0) c = -c;
        if (c > 2047) c = 2047;
        else if (c < -2048) c = -2048;
        blk[j] = (int16_t)c;
        mismatch ^= c;
        rows |= 1u << (j >> 3);
        if (j & 7) rows_ac |= 1u << (j >> 3);
    }
    b->pos = pos;
    state->mismatch = mismatch;
    state->rows = (uint8_t)rows;
    state->rows_ac = (uint8_t)rows_ac;
    return ok;
}

static bool intra_block(worker_t *w, int index, uint8_t *dst, ptrdiff_t stride) {
    PROF_START(w);
    struct mpeg2_dec *dec = w->dec;
    bits_t *b = &w->bits;
    const int cc = index < 4 ? 0 : index - 3;
    const int size = vlc_get(b, &dec->vlc_dc[cc ? 1 : 0]);
    if (size == VLC_ERROR) return false;
    int diff = 0;
    if (size) {
        const int v = (int)bits_u(b, (unsigned)size);
        diff = v >= (1 << (size - 1)) ? v : v + 1 - (1 << size);
    }
    const int dc = w->dc_pred[cc] + diff;
    w->dc_pred[cc] = dc;
    int16_t *blk = w->blk;
    blk[0] = (int16_t)(dc * (1 << (3 - dec->pic.dc_precision)));
    block_state_t st = { 1 ^ blk[0], 1, 0 };
    const bool ok = read_coefficients(w, &dec->vlc_dct[dec->pic.intra_vlc ? 1 : 0], 1, false,
                                      dec->intra_q, 0, &st);
    PROF_STOP(w, MPEG2_PROF_VLC);
    if (!ok) {
        memset(blk, 0, 64 * sizeof(int16_t));
        return false;
    }
    if (st.mismatch & 1) {
        blk[63] ^= 1;
        st.rows |= 0x80;
        st.rows_ac |= 0x80;
    }
    {
        PROF_START(w);
        mpeg2_idct_put(dst, stride, blk, st.rows, st.rows_ac);
        PROF_STOP(w, MPEG2_PROF_IDCT);
    }
    return true;
}

static bool inter_block(worker_t *w, uint8_t *dst, ptrdiff_t stride) {
    PROF_START(w);
    struct mpeg2_dec *dec = w->dec;
    int16_t *blk = w->blk;
    block_state_t st = { 1, 0, 0 };
    const bool ok = read_coefficients(w, &dec->vlc_dct[0], 0, true, dec->inter_q, 1, &st);
    PROF_STOP(w, MPEG2_PROF_VLC);
    if (!ok) {
        memset(blk, 0, 64 * sizeof(int16_t));
        return false;
    }
    if (st.mismatch & 1) {
        blk[63] ^= 1;
        st.rows |= 0x80;
        st.rows_ac |= 0x80;
    }
    {
        PROF_START(w);
        mpeg2_idct_add(dst, stride, blk, st.rows, st.rows_ac);
        PROF_STOP(w, MPEG2_PROF_IDCT);
    }
    return true;
}

static uint8_t *block_dst(worker_t *w, int mb_x, int index, ptrdiff_t *stride) {
    const struct mpeg2_dec *dec = w->dec;
    if (index < 4) {
        *stride = dec->ystride;
        return w->y + mb_x * 16 + (index & 1) * 8 + (index >> 1) * 8 * dec->ystride;
    }
    *stride = dec->cstride;
    return (index == 4 ? w->u : w->v) + mb_x * 8;
}

static void skipped_mb(worker_t *w, int mb_x, int row) {
    reset_dc(w);
    int mv[2][2] = { { 0, 0 }, { 0, 0 } };
    uint8_t flags = MB_FWD;
    if (w->dec->pic.type == MPEG2_PICTURE_P) {
        memset(w->pmv, 0, sizeof(w->pmv));
    } else {
        flags = w->prev_flags;
        mv[0][0] = w->pmv[0][0][0];
        mv[0][1] = w->pmv[0][0][1];
        mv[1][0] = w->pmv[0][1][0];
        mv[1][1] = w->pmv[0][1][1];
    }
    mpeg2_predict(w, mb_x, row, flags, mv);
}

static bool decode_mb(worker_t *w, int mb_x, int row) {
    struct mpeg2_dec *dec = w->dec;
    bits_t *b = &w->bits;
    const pic_t *pic = &dec->pic;
    const int type = vlc_get(b, &dec->vlc_mbtype[pic->type - 1]);
    if (type == VLC_ERROR) return false;
    uint8_t flags = (uint8_t)type;
    if (!pic->frame_pred_frame_dct) {
        const bool field_motion = (flags & (MB_FWD | MB_BWD)) && bits_u(b, 2) != 2;
        const bool field_dct = (flags & (MB_INTRA | MB_PATTERN)) && bits_u1(b);
        if (field_motion || field_dct) {
            atomic_store(&dec->unsupported, true);
            return false;
        }
    }
    if (flags & MB_QUANT) w->qscale = quantiser_scale(dec, bits_u(b, 5));

    if (flags & MB_INTRA) {
        if (pic->concealment_mv) {
            int mv[2];
            if (!read_motion(w, 0, mv)) return false;
            bits_skip(b, 1);
        } else {
            memset(w->pmv, 0, sizeof(w->pmv));
        }
        for (int i = 0; i < 6; i++) {
            ptrdiff_t stride;
            uint8_t *dst = block_dst(w, mb_x, i, &stride);
            if (!intra_block(w, i, dst, stride)) return false;
        }
        w->prev_flags = MB_INTRA;
        return true;
    }

    reset_dc(w);
    int mv[2][2] = { { 0, 0 }, { 0, 0 } };
    if (flags & MB_FWD) {
        if (!read_motion(w, 0, mv[0])) return false;
    } else if (pic->type == MPEG2_PICTURE_P) {
        memset(w->pmv, 0, sizeof(w->pmv));
        flags |= MB_FWD;
    }
    if ((flags & MB_BWD) && !read_motion(w, 1, mv[1])) return false;
    if (bits_overrun(b)) return false;

    int cbp = 0;
    if (flags & MB_PATTERN) {
        cbp = vlc_get(b, &dec->vlc_cbp);
        if (cbp == VLC_ERROR) return false;
    }
    mpeg2_predict(w, mb_x, row, flags, mv);
    for (int i = 0; i < 6; i++) {
        if (!(cbp & (32 >> i))) continue;
        ptrdiff_t stride;
        uint8_t *dst = block_dst(w, mb_x, i, &stride);
        if (!inter_block(w, dst, stride)) return false;
    }
    w->prev_flags = flags & (MB_FWD | MB_BWD);
    return true;
}

static bool decode_slice(worker_t *w, const slice_ref_t *s, int row, int *row_end) {
    struct mpeg2_dec *dec = w->dec;
    bits_t *b = &w->bits;
    mpeg2_bits_init(b, w->bits_buf, s->data, s->len);
    w->qscale = quantiser_scale(dec, bits_u(b, 5));
    if (bits_peek(b) >> 31) {
        bits_skip(b, 9);
        while (bits_u1(b)) bits_skip(b, 8);
    } else {
        bits_skip(b, 1);
    }
    reset_dc(w);
    memset(w->pmv, 0, sizeof(w->pmv));
    w->prev_flags = 0;

    int mb_x = -1;
    for (;;) {
        bits_reserve(b);
        int inc = 0;
        for (;;) {
            const int e = vlc_get(b, &dec->vlc_mbai);
            if (e == VLC_ERROR) return false;
            if (e == MBAI_ESCAPE) {
                inc += 33;
                continue;
            }
            inc += e;
            break;
        }
        if (mb_x < 0) {
            mb_x = inc - 1;
            if (mb_x < *row_end) return false;
        } else {
            if (dec->pic.type == MPEG2_PICTURE_I && inc > 1) return false;
            if (dec->pic.type == MPEG2_PICTURE_B && inc > 1 &&
                !(w->prev_flags & (MB_FWD | MB_BWD))) {
                return false;
            }
            for (int k = 1; k < inc; k++) {
                if (++mb_x >= dec->mb_w) return false;
                *row_end = mb_x + 1;
                skipped_mb(w, mb_x, row);
                w->done[mb_x] = 1;
            }
            mb_x++;
        }
        if (mb_x >= dec->mb_w) return false;
        *row_end = mb_x + 1;
        if (!decode_mb(w, mb_x, row)) return false;
        w->done[mb_x] = 1;
        if (bits_overrun(b)) return false;
        if ((bits_peek(b) >> 9) == 0) return true;
    }
}

static void pack_row(worker_t *w, int row) {
    struct mpeg2_dec *dec = w->dec;
    PROF_START(w);
    const int blocks = dec->width / 32;
    const int tail = (dec->width / 2) % 16;
    uint8_t *dst = dec->cur->data + (size_t)row * 16 * dec->packed_stride;
    for (int r = 0; r < 16; r++) {
        const uint8_t *c = (r & 1 ? w->v : w->u) + (r >> 1) * dec->cstride;
        const uint8_t *y = w->y + r * dec->ystride;
        uint8_t *out = dst + r * dec->packed_stride;
        vdec_k_pack(c, y, out, blocks);
        if (tail) {
            vdec_k_pack(c + blocks * 16, y + blocks * 32, w->scratch, 1);
            memcpy(out + blocks * 48, w->scratch, (size_t)tail * 3);
        }
    }
    PROF_STOP(w, MPEG2_PROF_FLUSH);
}

bool mpeg2_decode_row(worker_t *w, int row) {
    struct mpeg2_dec *dec = w->dec;
    memset(w->done, 0, dec->mb_w);
    bool ok = true;
    int row_end = 0;
    for (uint16_t i = dec->row_first[row]; i < dec->row_first[row + 1]; i++) {
        if (atomic_load(&dec->unsupported)) break;
        if (!decode_slice(w, &dec->slices[i], row, &row_end)) ok = false;
    }
    for (int x = 0; x < dec->mb_w; x++) {
        if (!w->done[x]) {
            mpeg2_conceal(w, x, row);
            ok = false;
        }
    }
    pack_row(w, row);
    return ok;
}
