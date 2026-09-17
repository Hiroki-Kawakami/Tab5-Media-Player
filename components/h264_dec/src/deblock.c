/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_internal.h"

static const uint8_t kAlpha[52] = {
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   4,   4,
    5,   6,   7,   8,   9,   10,  12,  13,  15,  17,  20,  22,  25,  28,  32,  36,  40,  45,
    50,  56,  63,  71,  80,  90,  101, 113, 127, 144, 162, 182, 203, 226, 255, 255,
};

static const uint8_t kBeta[52] = {
    0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  2,  2,
    2,  3,  3,  3,  3,  4,  4,  4,  6,  6,  7,  7,  8,  8,  9,  9,  10, 10,
    11, 11, 12, 12, 13, 13, 14, 14, 15, 15, 16, 16, 17, 17, 18, 18,
};

static const uint8_t kTc0[52][3] = {
    { 0, 0, 0 },    { 0, 0, 0 },    { 0, 0, 0 },    { 0, 0, 0 },    { 0, 0, 0 },
    { 0, 0, 0 },    { 0, 0, 0 },    { 0, 0, 0 },    { 0, 0, 0 },    { 0, 0, 0 },
    { 0, 0, 0 },    { 0, 0, 0 },    { 0, 0, 0 },    { 0, 0, 0 },    { 0, 0, 0 },
    { 0, 0, 0 },    { 0, 0, 0 },    { 0, 0, 1 },    { 0, 0, 1 },    { 0, 0, 1 },
    { 0, 0, 1 },    { 0, 1, 1 },    { 0, 1, 1 },    { 1, 1, 1 },    { 1, 1, 1 },
    { 1, 1, 1 },    { 1, 1, 1 },    { 1, 1, 2 },    { 1, 1, 2 },    { 1, 1, 2 },
    { 1, 1, 2 },    { 1, 2, 3 },    { 1, 2, 3 },    { 2, 2, 3 },    { 2, 2, 4 },
    { 2, 3, 4 },    { 2, 3, 4 },    { 3, 3, 5 },    { 3, 4, 6 },    { 3, 4, 6 },
    { 4, 5, 7 },    { 4, 5, 8 },    { 4, 6, 9 },    { 5, 7, 10 },   { 6, 8, 11 },
    { 6, 8, 13 },   { 7, 10, 14 },  { 8, 11, 16 },  { 9, 12, 18 },  { 10, 13, 20 },
    { 11, 15, 23 }, { 13, 17, 25 },
};

static inline bool mb_intra(const mbinfo_t *m) {
    return m->kind != MB_INTER && m->kind != MB_SKIP;
}

static inline bool motion_differs(const mbinfo_t *p, int pb, const mbinfo_t *q, int qb) {
    const int p8 = ((pb >> 3) << 1) | ((pb & 3) >> 1);
    const int q8 = ((qb >> 3) << 1) | ((qb & 3) >> 1);
    return p->refpic[p8] != q->refpic[q8] || iabs(p->mv[pb][0] - q->mv[qb][0]) >= 4 ||
           iabs(p->mv[pb][1] - q->mv[qb][1]) >= 4;
}

static uint8_t strength(const mbinfo_t *p, int pb, const mbinfo_t *q, int qb) {
    if (((p->nzmask >> pb) | (q->nzmask >> qb)) & 1) return 2;
    return motion_differs(p, pb, q, qb);
}

static bool edge_strengths(const mbinfo_t *p, const mbinfo_t *q, int e, bool vertical,
                           uint8_t *bs) {
    const bool mb_edge = e == 0;
    if (mb_intra(p) || mb_intra(q)) {
        const uint8_t v = mb_edge ? 4 : 3;
        bs[0] = bs[1] = bs[2] = bs[3] = v;
        return true;
    }
    uint8_t any = 0;
    if (p->uniform && q->uniform && !p->nzmask && !q->nzmask) {
        if (!mb_edge) return false;
        const uint8_t v = motion_differs(p, 0, q, 0);
        bs[0] = bs[1] = bs[2] = bs[3] = v;
        return v != 0;
    }
    for (int k = 0; k < 4; k++) {
        int qb, pb;
        if (vertical) {
            qb = k * 4 + e;
            pb = mb_edge ? k * 4 + 3 : qb - 1;
        } else {
            qb = e * 4 + k;
            pb = mb_edge ? 12 + k : qb - 4;
        }
        bs[k] = strength(p, pb, q, qb);
        any |= bs[k];
    }
    return any != 0;
}

static void filter_luma(uint8_t *pix, ptrdiff_t across, ptrdiff_t along, const uint8_t *bs,
                        int index_a, int beta) {
    const int alpha = kAlpha[index_a];
    if (!alpha || !beta) return;
    for (int seg = 0; seg < 4; seg++) {
        const int s = bs[seg];
        if (!s) {
            pix += 4 * along;
            continue;
        }
        const int tc0 = s < 4 ? kTc0[index_a][s - 1] : 0;
        for (int k = 0; k < 4; k++, pix += along) {
            const int p0 = pix[-across], p1 = pix[-2 * across], p2 = pix[-3 * across];
            const int q0 = pix[0], q1 = pix[across], q2 = pix[2 * across];
            if (iabs(p0 - q0) >= alpha || iabs(p1 - p0) >= beta || iabs(q1 - q0) >= beta) continue;
            const int ap = iabs(p2 - p0);
            const int aq = iabs(q2 - q0);
            if (s < 4) {
                int tc = tc0;
                if (ap < beta) {
                    pix[-2 * across] = (uint8_t)(p1 + clip3(-tc0, tc0, (p2 + ((p0 + q0 + 1) >> 1) - (p1 << 1)) >> 1));
                    tc++;
                }
                if (aq < beta) {
                    pix[across] = (uint8_t)(q1 + clip3(-tc0, tc0, (q2 + ((p0 + q0 + 1) >> 1) - (q1 << 1)) >> 1));
                    tc++;
                }
                const int delta = clip3(-tc, tc, (((q0 - p0) * 4) + (p1 - q1) + 4) >> 3);
                pix[-across] = clip_u8(p0 + delta);
                pix[0] = clip_u8(q0 - delta);
            } else {
                const int p3 = pix[-4 * across], q3 = pix[3 * across];
                const bool strong = iabs(p0 - q0) < ((alpha >> 2) + 2);
                if (ap < beta && strong) {
                    pix[-across] = (uint8_t)((p2 + 2 * p1 + 2 * p0 + 2 * q0 + q1 + 4) >> 3);
                    pix[-2 * across] = (uint8_t)((p2 + p1 + p0 + q0 + 2) >> 2);
                    pix[-3 * across] = (uint8_t)((2 * p3 + 3 * p2 + p1 + p0 + q0 + 4) >> 3);
                } else {
                    pix[-across] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
                }
                if (aq < beta && strong) {
                    pix[0] = (uint8_t)((p1 + 2 * p0 + 2 * q0 + 2 * q1 + q2 + 4) >> 3);
                    pix[across] = (uint8_t)((p0 + q0 + q1 + q2 + 2) >> 2);
                    pix[2 * across] = (uint8_t)((2 * q3 + 3 * q2 + q1 + q0 + p0 + 4) >> 3);
                } else {
                    pix[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
                }
            }
        }
    }
}

static void filter_chroma(uint8_t *pix, ptrdiff_t across, ptrdiff_t along, const uint8_t *bs,
                          int index_a, int beta) {
    const int alpha = kAlpha[index_a];
    if (!alpha || !beta) return;
    for (int k = 0; k < 8; k++, pix += along) {
        const int s = bs[k >> 1];
        if (!s) continue;
        const int p0 = pix[-across], p1 = pix[-2 * across];
        const int q0 = pix[0], q1 = pix[across];
        if (iabs(p0 - q0) >= alpha || iabs(p1 - p0) >= beta || iabs(q1 - q0) >= beta) continue;
        if (s < 4) {
            const int tc = kTc0[index_a][s - 1] + 1;
            const int delta = clip3(-tc, tc, (((q0 - p0) * 4) + (p1 - q1) + 4) >> 3);
            pix[-across] = clip_u8(p0 + delta);
            pix[0] = clip_u8(q0 - delta);
        } else {
            pix[-across] = (uint8_t)((2 * p1 + p0 + q1 + 2) >> 2);
            pix[0] = (uint8_t)((2 * q1 + q0 + p1 + 2) >> 2);
        }
    }
}

static void edge(uint8_t *luma, uint8_t *cb, uint8_t *cr, ptrdiff_t l_across, ptrdiff_t l_along,
                 ptrdiff_t c_across, ptrdiff_t c_along, const uint8_t *bs, int chroma_edge,
                 const mbinfo_t *p, const mbinfo_t *q) {
    if (!(bs[0] | bs[1] | bs[2] | bs[3])) return;
    const int qp = (p->qp + q->qp + 1) >> 1;
    const int ia = clip3(0, 51, qp + q->alpha);
    const int ib = clip3(0, 51, qp + q->beta);
    filter_luma(luma, l_across, l_along, bs, ia, kBeta[ib]);
    if (!chroma_edge) return;
    for (int c = 0; c < 2; c++) {
        const int cq = (p->cqp[c] + q->cqp[c] + 1) >> 1;
        const int ca = clip3(0, 51, cq + q->alpha);
        const int cb_ = clip3(0, 51, cq + q->beta);
        filter_chroma(c ? cr : cb, c_across, c_along, bs, ca, kBeta[cb_]);
    }
}

void h264_deblock_row(struct h264_dec *dec, rowbuf_t *cur, rowbuf_t *above, uint32_t mb_y) {
    const ptrdiff_t ls = dec->luma_stride;
    const ptrdiff_t cs = dec->chroma_stride;
    uint8_t bs[4];
    for (uint32_t mb_x = 0; mb_x < dec->mb_w; mb_x++) {
        const mbinfo_t *q = &cur->mb[mb_x];
        if (q->filter == 1) continue;
        uint8_t *y = cur->y + mb_x * 16;
        uint8_t *u = cur->u + mb_x * 8;
        uint8_t *v = cur->v + mb_x * 8;
        const mbinfo_t *left = mb_x > 0 ? &cur->mb[mb_x - 1] : NULL;
        const mbinfo_t *top = mb_y > 0 && above ? &above->mb[mb_x] : NULL;
        if (left && q->filter == 2 && left->slice != q->slice) left = NULL;
        if (top && q->filter == 2 && top->slice != q->slice) top = NULL;
        const bool internal = mb_intra(q) || !q->uniform || q->nzmask;

        if (left && edge_strengths(left, q, 0, true, bs)) {
            edge(y, u, v, 1, ls, 1, cs, bs, 1, left, q);
        }
        if (internal) {
            for (int e = 1; e < 4; e++) {
                if (edge_strengths(q, q, e, true, bs)) {
                    edge(y + e * 4, u + e * 2, v + e * 2, 1, ls, 1, cs, bs, !(e & 1), q, q);
                }
            }
        }
        if (top && edge_strengths(top, q, 0, false, bs)) {
            edge(y, u, v, ls, 1, cs, 1, bs, 1, top, q);
        }
        if (internal) {
            for (int e = 1; e < 4; e++) {
                if (edge_strengths(q, q, e, false, bs)) {
                    edge(y + e * 4 * ls, u + e * 2 * cs, v + e * 2 * cs, ls, 1, cs, 1, bs, !(e & 1),
                         q, q);
                }
            }
        }
    }
}
