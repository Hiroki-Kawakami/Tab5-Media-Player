/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "mpeg2_internal.h"
#include "vdec_kernels.h"

static void fetch_luma(worker_t *w, const frame_t *ref, int x0, int y0, int rows) {
    const struct mpeg2_dec *dec = w->dec;
    const uint32_t S = dec->packed_stride;
    uint8_t *dst = w->ref_y;
    if (x0 >= 0 && y0 >= 0 && x0 + 17 <= dec->width && y0 + rows <= dec->height) {
        vdec_k_unpack_rows(ref->data + (size_t)y0 * S + 3 * (x0 >> 1), S, w->scratch, 0,
                           dst - (x0 & 1), MC_STRIDE, rows);
        return;
    }
    const int W = dec->width, H = dec->height;
    for (int r = 0; r < rows; r++, dst += MC_STRIDE) {
        const uint8_t *row = ref->data + (size_t)clip3(0, H - 1, y0 + r) * S;
        for (int c = 0; c < 17; c++) {
            const int xx = clip3(0, W - 1, x0 + c);
            dst[c] = row[3 * (xx >> 1) + 1 + (xx & 1)];
        }
    }
}

static void fetch_chroma(worker_t *w, const frame_t *ref, int comp, int x0, int y0, int rows,
                         uint8_t *dst) {
    const struct mpeg2_dec *dec = w->dec;
    const uint32_t S = dec->packed_stride;
    const int W = dec->width / 2, H = dec->height / 2;
    if (x0 >= 0 && y0 >= 0 && x0 + 9 <= W && y0 + rows <= H) {
        vdec_k_unpack_rows(ref->data + ((size_t)y0 * 2 + comp) * S + 3 * x0, 2 * S, dst, 16,
                           w->scratch, 0, rows);
        return;
    }
    for (int r = 0; r < rows; r++, dst += 16) {
        const uint8_t *row = ref->data + ((size_t)clip3(0, H - 1, y0 + r) * 2 + comp) * S;
        for (int c = 0; c < 9; c++) dst[c] = row[3 * clip3(0, W - 1, x0 + c)];
    }
}

static void interpolate(const uint8_t *src, ptrdiff_t ss, int hx, int hy, int width, int rows,
                        uint8_t *dst, ptrdiff_t ds) {
    if (!hx && !hy) {
        vdec_k_copy(src, ss, dst, ds, rows);
    } else if (hx && !hy) {
        vdec_k_avg_u8(src, src + 1, dst, rows, ss, ss, ds);
    } else if (!hx) {
        vdec_k_avg_u8(src, src + ss, dst, rows, ss, ss, ds);
    } else {
        vdec_k_bilinear(src, ss, dst, ds, rows, 4, 4);
        if (width > 8) vdec_k_bilinear(src + 8, ss, dst + 8, ds, rows, 4, 4);
    }
}

static void predict_one(worker_t *w, const frame_t *ref, int mb_x, int row, const int mv[2],
                        uint8_t *dy, ptrdiff_t ys, uint8_t *du, uint8_t *dv, ptrdiff_t cs) {
    {
        PROF_START(w);
        fetch_luma(w, ref, mb_x * 16 + (mv[0] >> 1), row * 16 + (mv[1] >> 1), 16 + (mv[1] & 1));
        PROF_STOP(w, MPEG2_PROF_FETCH);
    }
    interpolate(w->ref_y, MC_STRIDE, mv[0] & 1, mv[1] & 1, 16, 16, dy, ys);

    const int cx = mv[0] / 2;
    const int cy = mv[1] / 2;
    const int x0 = mb_x * 8 + (cx >> 1);
    const int y0 = row * 8 + (cy >> 1);
    const int rows = 8 + (cy & 1);
    {
        PROF_START(w);
        fetch_chroma(w, ref, 0, x0, y0, rows, w->ref_u);
        fetch_chroma(w, ref, 1, x0, y0, rows, w->ref_v);
        PROF_STOP(w, MPEG2_PROF_FETCH);
    }
    interpolate(w->ref_u, 16, cx & 1, cy & 1, 8, 8, du, cs);
    interpolate(w->ref_v, 16, cx & 1, cy & 1, 8, 8, dv, cs);
}

void mpeg2_predict(worker_t *w, int mb_x, int row, uint8_t flags, const int mv[2][2]) {
    PROF_START(w);
    const struct mpeg2_dec *dec = w->dec;
    const ptrdiff_t ys = dec->ystride, cs = dec->cstride;
    uint8_t *dy = w->y + mb_x * 16;
    uint8_t *du = w->u + mb_x * 8;
    uint8_t *dv = w->v + mb_x * 8;
    const bool fwd = flags & MB_FWD;
    const bool bwd = flags & MB_BWD;
    if (fwd && bwd) {
        predict_one(w, dec->pred_ref[0], mb_x, row, mv[0], w->pred_y[0], MC_STRIDE, w->pred_u[0],
                    w->pred_v[0], 16);
        predict_one(w, dec->pred_ref[1], mb_x, row, mv[1], w->pred_y[1], MC_STRIDE, w->pred_u[1],
                    w->pred_v[1], 16);
        vdec_k_avg_u8(w->pred_y[0], w->pred_y[1], dy, 16, MC_STRIDE, MC_STRIDE, ys);
        vdec_k_avg_u8(w->pred_u[0], w->pred_u[1], du, 8, 16, 16, cs);
        vdec_k_avg_u8(w->pred_v[0], w->pred_v[1], dv, 8, 16, 16, cs);
    } else {
        const int s = bwd ? 1 : 0;
        predict_one(w, dec->pred_ref[s], mb_x, row, mv[s], dy, ys, du, dv, cs);
    }
    PROF_STOP(w, MPEG2_PROF_MC);
}

void mpeg2_conceal(worker_t *w, int mb_x, int row) {
    const struct mpeg2_dec *dec = w->dec;
    w->concealed = true;
    uint8_t *dy = w->y + mb_x * 16;
    uint8_t *du = w->u + mb_x * 8;
    uint8_t *dv = w->v + mb_x * 8;
    if (!dec->conceal_ref) {
        for (int r = 0; r < 16; r++) memset(dy + r * dec->ystride, 128, 16);
        for (int r = 0; r < 8; r++) {
            memset(du + r * dec->cstride, 128, 8);
            memset(dv + r * dec->cstride, 128, 8);
        }
        return;
    }
    static const int zero[2] = { 0, 0 };
    predict_one(w, dec->conceal_ref, mb_x, row, zero, w->pred_y[0], MC_STRIDE, w->pred_u[0],
                w->pred_v[0], 16);
    for (int r = 0; r < 16; r++) memcpy(dy + r * dec->ystride, w->pred_y[0] + r * MC_STRIDE, 16);
    for (int r = 0; r < 8; r++) {
        memcpy(du + r * dec->cstride, w->pred_u[0] + r * 16, 8);
        memcpy(dv + r * dec->cstride, w->pred_v[0] + r * 16, 8);
    }
}
