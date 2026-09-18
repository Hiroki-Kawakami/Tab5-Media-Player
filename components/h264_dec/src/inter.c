/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_internal.h"
#include "kernels.h"

enum { TS = MC_TMP_STRIDE };

static void window_store(struct h264_dec *dec, const frame_t *f, int y) {
    const size_t w = dec->width;
    const size_t cw = w / 2;
    const int blocks = (int)((cw + 15) / 16);
    const int slot = y & (WINDOW_ROWS - 1);
    const int cy = y >> 1;
    const int cslot = cy & (WINDOW_ROWS / 2 - 1);
    uint8_t *c = ((y & 1) ? dec->win_v : dec->win_u) + (size_t)cslot * dec->win_cstride;
    uint8_t *luma = dec->win_y + (size_t)slot * dec->win_stride;
    h264_k_unpack(f->data + (size_t)y * dec->packed_stride, c, luma, blocks);
    if (slot < WINDOW_MIRROR) memcpy(luma + (size_t)WINDOW_ROWS * dec->win_stride, luma, w);
    if (cslot < WINDOW_MIRROR / 2) {
        memcpy(c + (size_t)(WINDOW_ROWS / 2) * dec->win_cstride, c, cw);
    }
}

bool h264_window_fill_one(struct h264_dec *dec, int limit, atomic_int *busy) {
    int y = atomic_load(&dec->win_claimed);
    for (;;) {
        const int cap = imin(limit, atomic_load(&dec->win_floor) + WINDOW_ROWS);
        if (y >= cap) {
            if (busy) atomic_store(busy, -1);
            return false;
        }
        if (busy) atomic_store(busy, y);
        if (atomic_compare_exchange_weak(&dec->win_claimed, &y, y + 1)) break;
    }
    window_store(dec, atomic_load(&dec->win_source), y);
    if (busy) atomic_store(busy, -1);
    return true;
}

void h264_window_fill(struct h264_dec *dec) {
    const h264_dec_threads_t *t = dec->config.threads;
    PROF_START(dec);
    while (h264_window_fill_one(dec, atomic_load(&dec->win_goal), &dec->win_busy)) {
        if (dec->threaded) t->sem_give(t->ctx, dec->sem_window);
    }
    PROF_STOP(dec, H264_PROF_WINDOW);
}

void h264_window_advance(struct h264_dec *dec, uint32_t mb_y) {
    if (!dec->win_y || dec->win_disabled) return;
    const frame_t *f = dec->ref_count[0] ? dec->ref_list[0][0] : NULL;
    if (!f || f->non_existing || (dec->win_frame && f != dec->win_frame)) {
        h264_window_wait(dec, dec->win_target);
        dec->win_disabled = true;
        dec->win_frame = NULL;
        return;
    }
    const int lo = imax(0, (int)mb_y * 16 - WINDOW_MARGIN) & ~1;
    const int hi = imin(dec->height, (int)mb_y * 16 + 16 + WINDOW_MARGIN);
    if (!dec->win_frame) {
        dec->win_frame = f;
        atomic_store(&dec->win_source, f);
        atomic_store(&dec->win_floor, lo);
        atomic_store(&dec->win_claimed, lo);
        atomic_store(&dec->win_busy, -1);
        dec->win_target = lo;
    } else if (lo < dec->win_lo) {
        return;
    }
    atomic_store(&dec->win_floor, lo);
    dec->win_lo = lo;
    const int ahead = imin(imin(dec->height, hi + 16), lo + WINDOW_ROWS);
    if (ahead > dec->win_target) {
        dec->win_target = ahead;
        h264_window_request(dec, ahead);
    }
    h264_window_wait(dec, hi);
    dec->win_hi = hi;
}

static void fetch_luma(struct h264_dec *dec, const frame_t *ref, int x0, int y0, int w, int h,
                       uint8_t *dst, ptrdiff_t dst_stride) {
    PROF_START(dec);
    const int W = dec->width, H = dec->height;
    const uint32_t S = dec->packed_stride;
    for (int r = 0; r < h; r++, dst += dst_stride) {
        const int yy = clip3(0, H - 1, y0 + r);
        const uint8_t *win = h264_window_luma(dec, ref, yy, 1);
        const uint8_t *row = ref->data + (size_t)yy * S;
        for (int c = 0; c < w; c++) {
            const int xx = clip3(0, W - 1, x0 + c);
            dst[c] = win ? win[xx] : row[3 * (xx >> 1) + 1 + (xx & 1)];
        }
    }
    PROF_STOP(dec, H264_PROF_MC_FETCH);
}

static void fetch_luma_inside(struct h264_dec *dec, const frame_t *ref, int x0, int y0, int h,
                              uint8_t *dst, ptrdiff_t dst_stride) {
    PROF_START(dec);
    const uint32_t S = dec->packed_stride;
    h264_k_unpack_rows(ref->data + (size_t)y0 * S + 3 * (x0 >> 1), S, dec->mc_scratch, 0,
                       dst - (x0 & 1), dst_stride, h);
    PROF_STOP(dec, H264_PROF_MC_FETCH);
}

static void fetch_chroma(struct h264_dec *dec, const frame_t *ref, int comp, int x0, int y0,
                         int w, int h, uint8_t *dst, ptrdiff_t dst_stride) {
    const int W = dec->width / 2, H = dec->height / 2;
    if (x0 >= 0 && x0 + w <= W && y0 >= 0 && y0 + h <= H) {
        const uint32_t S = dec->packed_stride;
        h264_k_unpack_rows(ref->data + ((size_t)y0 * 2 + comp) * S + 3 * x0, 2 * S, dst, dst_stride,
                           dec->mc_scratch, 0, h);
        return;
    }
    for (int r = 0; r < h; r++, dst += dst_stride) {
        const int yy = clip3(0, H - 1, y0 + r);
        const uint8_t *win = h264_window_chroma(dec, ref, comp, yy, 1);
        const uint8_t *row = ref->data + ((size_t)yy * 2 + comp) * dec->packed_stride;
        for (int c = 0; c < w; c++) {
            const int xx = clip3(0, W - 1, x0 + c);
            dst[c] = win ? win[xx] : row[3 * xx];
        }
    }
}

static void hpel_hv(struct h264_dec *dec, const uint8_t *src, ptrdiff_t ss, int h, uint8_t *dst,
                    ptrdiff_t stride) {
    int16_t *mid = dec->mc_mid;
    h264_k_tap6_s16(src, mid, h + 5, ss);
    h264_k_tap6v_s16(mid, dst, h, stride);
}

static void interpolate(struct h264_dec *dec, const uint8_t *r0, ptrdiff_t ss, int frac, int h,
                        uint8_t *dst, ptrdiff_t stride) {
    const uint8_t *r2 = r0 + 2 * ss;
    const uint8_t *r3 = r0 + 3 * ss;
    uint8_t *pa = dec->mc_a;
    uint8_t *pb = dec->mc_b;
    switch (frac) {
    case 1:
        h264_k_tap6_u8(r2, 1, pa, h, ss, 16);
        h264_k_avg_u8(r2 + 2, pa, dst, h, ss, 16, stride);
        break;
    case 2:
        h264_k_tap6_u8(r2, 1, dst, h, ss, stride);
        break;
    case 3:
        h264_k_tap6_u8(r2, 1, pa, h, ss, 16);
        h264_k_avg_u8(r2 + 3, pa, dst, h, ss, 16, stride);
        break;
    case 4:
        h264_k_tap6_u8(r0 + 2, ss, pa, h, ss, 16);
        h264_k_avg_u8(r2 + 2, pa, dst, h, ss, 16, stride);
        break;
    case 8:
        h264_k_tap6_u8(r0 + 2, ss, dst, h, ss, stride);
        break;
    case 12:
        h264_k_tap6_u8(r0 + 2, ss, pa, h, ss, 16);
        h264_k_avg_u8(r3 + 2, pa, dst, h, ss, 16, stride);
        break;
    case 5:
        h264_k_tap6_u8(r2, 1, pa, h, ss, 16);
        h264_k_tap6_u8(r0 + 2, ss, pb, h, ss, 16);
        h264_k_avg_u8(pa, pb, dst, h, 16, 16, stride);
        break;
    case 7:
        h264_k_tap6_u8(r2, 1, pa, h, ss, 16);
        h264_k_tap6_u8(r0 + 3, ss, pb, h, ss, 16);
        h264_k_avg_u8(pa, pb, dst, h, 16, 16, stride);
        break;
    case 13:
        h264_k_tap6_u8(r3, 1, pa, h, ss, 16);
        h264_k_tap6_u8(r0 + 2, ss, pb, h, ss, 16);
        h264_k_avg_u8(pa, pb, dst, h, 16, 16, stride);
        break;
    case 15:
        h264_k_tap6_u8(r3, 1, pa, h, ss, 16);
        h264_k_tap6_u8(r0 + 3, ss, pb, h, ss, 16);
        h264_k_avg_u8(pa, pb, dst, h, 16, 16, stride);
        break;
    case 10:
        hpel_hv(dec, r0, ss, h, dst, stride);
        break;
    default:
        hpel_hv(dec, r0, ss, h, pb, 16);
        switch (frac) {
        case 6: h264_k_tap6_u8(r2, 1, pa, h, ss, 16); break;
        case 14: h264_k_tap6_u8(r3, 1, pa, h, ss, 16); break;
        case 9: h264_k_tap6_u8(r0 + 2, ss, pa, h, ss, 16); break;
        default: h264_k_tap6_u8(r0 + 3, ss, pa, h, ss, 16); break;
        }
        h264_k_avg_u8(pb, pa, dst, h, 16, 16, stride);
        break;
    }
}

void h264_weight_block(uint8_t *dst, ptrdiff_t stride, int h, int weight, int offset, int denom) {
    const int round = denom ? 1 << (denom - 1) : 0;
    h264_k_weight_u8(dst, stride, h, weight, round, denom, offset);
}

void h264_avg_block(uint8_t *dst, ptrdiff_t stride, const uint8_t *a, const uint8_t *b,
                    ptrdiff_t src_stride, int h) {
    h264_k_avg_u8(a, b, dst, h, src_stride, src_stride, stride);
}

void h264_weight_bi_block(uint8_t *dst, ptrdiff_t stride, const uint8_t *a, const uint8_t *b,
                          ptrdiff_t src_stride, int w, int h, int w0, int w1, int offset,
                          int denom) {
    const int round = 1 << denom;
    const int shift = denom + 1;
    for (int y = 0; y < h; y++, a += src_stride, b += src_stride, dst += stride) {
        for (int x = 0; x < w; x++) {
            dst[x] = clip_u8(((a[x] * w0 + b[x] * w1 + round) >> shift) + offset);
        }
    }
}

void h264_weight_bi_implicit(uint8_t *dst, ptrdiff_t stride, const uint8_t *a, const uint8_t *b,
                             ptrdiff_t src_stride, int h, int w1) {
    h264_k_weight_bi_u8(a, b, dst, h, src_stride, stride, w1);
}

void h264_mc_luma(struct h264_dec *dec, const frame_t *ref, uint8_t *dst, ptrdiff_t stride,
                  int x, int y, int w, int h, int mvx, int mvy) {
    const int xi = x + (mvx >> 2);
    const int yi = y + (mvy >> 2);
    const int frac = ((mvy & 3) << 2) | (mvx & 3);
    const int W = dec->width, H = dec->height;
    const ptrdiff_t ws = dec->win_stride;

    if (!frac) {
        if (xi >= 0 && xi + w <= W && yi >= 0 && yi + h <= H) {
            const uint8_t *src = h264_window_luma(dec, ref, yi, h);
            if (src) {
                src += xi;
                h264_k_copy(src, ws, dst, stride, h);
                return;
            }
            if (xi & 1) {
                fetch_luma_inside(dec, ref, xi, yi, h, dec->mc_src, TS);
                h264_k_copy(dec->mc_src, TS, dst, stride, h);
            } else {
                fetch_luma_inside(dec, ref, xi, yi, h, dst, stride);
            }
            return;
        }
        fetch_luma(dec, ref, xi, yi, w, h, dst, stride);
        return;
    }

    const bool inside = xi >= 2 && xi + w + 3 <= W && yi >= 2 && yi + h + 3 <= H;
    if (inside) {
        const uint8_t *src = h264_window_luma(dec, ref, yi - 2, h + 5);
        if (src) {
            interpolate(dec, src + xi - 2, ws, frac, h, dst, stride);
            return;
        }
        fetch_luma_inside(dec, ref, xi - 2, yi - 2, h + 5, dec->mc_src, TS);
    } else {
        fetch_luma(dec, ref, xi - 2, yi - 2, w + 5, h + 5, dec->mc_src, TS);
    }
    interpolate(dec, dec->mc_src, TS, frac, h, dst, stride);
}

void h264_mc_chroma(struct h264_dec *dec, const frame_t *ref, uint8_t *dst_u, uint8_t *dst_v,
                    ptrdiff_t stride, int x, int y, int w, int h, int mvx, int mvy) {
    const int xi = x + (mvx >> 3);
    const int yi = y + (mvy >> 3);
    const int fx = mvx & 7;
    const int fy = mvy & 7;
    const int CW = dec->width / 2, CH = dec->height / 2;
    const int m = (fx | fy) ? 1 : 0;
    const bool inside = xi >= 0 && xi + w + m <= CW && yi >= 0 && yi + h + m <= CH;
    for (int comp = 0; comp < 2; comp++) {
        uint8_t *dst = comp ? dst_v : dst_u;
        const uint8_t *src = inside ? h264_window_chroma(dec, ref, comp, yi, h + m) : NULL;
        ptrdiff_t ss = dec->win_cstride;
        if (src) {
            src += xi;
        } else {
            fetch_chroma(dec, ref, comp, xi, yi, w + m, h + m, dec->mc_chroma, 16);
            src = dec->mc_chroma;
            ss = 16;
        }
        if (!fx && !fy) {
            h264_k_copy(src, ss, dst, stride, h);
        } else {
            h264_k_bilinear(src, ss, dst, stride, h, fx, fy);
        }
    }
}
