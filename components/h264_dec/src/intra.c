/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_internal.h"

#define AVG2(a, b) (uint8_t)(((a) + (b) + 1) >> 1)
#define AVG3(a, b, c) (uint8_t)(((a) + 2 * (b) + (c) + 2) >> 2)

void h264_intra4x4(uint8_t *dst, ptrdiff_t stride, int mode, unsigned avail) {
    uint8_t top_buf[9];
    uint8_t left_buf[5];
    const uint8_t *T = top_buf + 1;
    const uint8_t *L = left_buf + 1;
    const uint8_t *above = dst - stride;
    top_buf[0] = above[-1];
    left_buf[0] = above[-1];
    for (int i = 0; i < 4; i++) {
        top_buf[1 + i] = above[i];
        left_buf[1 + i] = dst[i * stride - 1];
    }
    for (int i = 4; i < 8; i++) {
        top_buf[1 + i] = (avail & AVAIL_TOP_RIGHT) ? above[i] : above[3];
    }

    uint8_t p[16];
    switch (mode) {
    case 0:
        for (int y = 0; y < 4; y++) memcpy(p + y * 4, T, 4);
        break;
    case 1:
        for (int y = 0; y < 4; y++) memset(p + y * 4, L[y], 4);
        break;
    case 2: {
        int dc;
        const int st = T[0] + T[1] + T[2] + T[3];
        const int sl = L[0] + L[1] + L[2] + L[3];
        const bool l = avail & AVAIL_LEFT, t = avail & AVAIL_TOP;
        if (l && t) dc = (st + sl + 4) >> 3;
        else if (l) dc = (sl + 2) >> 2;
        else if (t) dc = (st + 2) >> 2;
        else dc = 128;
        memset(p, dc, 16);
        break;
    }
    case 3:
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                p[y * 4 + x] = (x == 3 && y == 3) ? (uint8_t)((T[6] + 3 * T[7] + 2) >> 2)
                                                  : AVG3(T[x + y], T[x + y + 1], T[x + y + 2]);
            }
        }
        break;
    case 4:
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                if (x > y) p[y * 4 + x] = AVG3(T[x - y - 2], T[x - y - 1], T[x - y]);
                else if (x < y) p[y * 4 + x] = AVG3(L[y - x - 2], L[y - x - 1], L[y - x]);
                else p[y * 4 + x] = AVG3(T[0], T[-1], L[0]);
            }
        }
        break;
    case 5:
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                const int z = 2 * x - y;
                const int i = x - (y >> 1);
                uint8_t v;
                if (z >= 0 && !(z & 1)) v = AVG2(T[i - 1], T[i]);
                else if (z >= 0) v = AVG3(T[i - 2], T[i - 1], T[i]);
                else if (z == -1) v = AVG3(L[0], L[-1], T[0]);
                else v = AVG3(L[y - 1], L[y - 2], L[y - 3]);
                p[y * 4 + x] = v;
            }
        }
        break;
    case 6:
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                const int z = 2 * y - x;
                const int i = y - (x >> 1);
                uint8_t v;
                if (z >= 0 && !(z & 1)) v = AVG2(L[i - 1], L[i]);
                else if (z >= 0) v = AVG3(L[i - 2], L[i - 1], L[i]);
                else if (z == -1) v = AVG3(L[0], L[-1], T[0]);
                else v = AVG3(T[x - 1], T[x - 2], T[x - 3]);
                p[y * 4 + x] = v;
            }
        }
        break;
    case 7:
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                const int i = x + (y >> 1);
                p[y * 4 + x] = (y & 1) ? AVG3(T[i], T[i + 1], T[i + 2]) : AVG2(T[i], T[i + 1]);
            }
        }
        break;
    default: {
        for (int y = 0; y < 4; y++) {
            for (int x = 0; x < 4; x++) {
                const int z = x + 2 * y;
                const int i = y + (x >> 1);
                uint8_t v;
                if (z > 5) v = L[3];
                else if (z == 5) v = (uint8_t)((L[2] + 3 * L[3] + 2) >> 2);
                else if (z & 1) v = AVG3(L[i], L[i + 1], L[i + 2]);
                else v = AVG2(L[i], L[i + 1]);
                p[y * 4 + x] = v;
            }
        }
        break;
    }
    }
    for (int y = 0; y < 4; y++) memcpy(dst + y * stride, p + y * 4, 4);
}

static void plane_predict(uint8_t *dst, ptrdiff_t stride, int size, int hv_scale) {
    const uint8_t *above = dst - stride;
    const int half = size / 2;
    int H = 0, V = 0;
    for (int k = 0; k < half; k++) {
        const int top_a = above[half + k];
        const int top_b = above[half - 2 - k];
        const int left_a = dst[(half + k) * stride - 1];
        const int left_b = (half - 2 - k) >= 0 ? dst[(half - 2 - k) * stride - 1] : above[-1];
        H += (k + 1) * (top_a - top_b);
        V += (k + 1) * (left_a - left_b);
    }
    const int a = 16 * (dst[(size - 1) * stride - 1] + above[size - 1]);
    const int b = (hv_scale * H + 32) >> 6;
    const int c = (hv_scale * V + 32) >> 6;
    const int center = half - 1;
    for (int y = 0; y < size; y++) {
        uint8_t *row = dst + y * stride;
        int acc = a + b * (0 - center) + c * (y - center) + 16;
        for (int x = 0; x < size; x++, acc += b) row[x] = clip_u8(acc >> 5);
    }
}

void h264_intra16x16(uint8_t *dst, ptrdiff_t stride, int mode, unsigned avail) {
    const uint8_t *above = dst - stride;
    switch (mode) {
    case 0:
        for (int y = 0; y < 16; y++) memcpy(dst + y * stride, above, 16);
        break;
    case 1:
        for (int y = 0; y < 16; y++) memset(dst + y * stride, dst[y * stride - 1], 16);
        break;
    case 2: {
        int st = 0, sl = 0;
        for (int i = 0; i < 16; i++) {
            st += above[i];
            sl += dst[i * stride - 1];
        }
        const bool l = avail & AVAIL_LEFT, t = avail & AVAIL_TOP;
        int dc;
        if (l && t) dc = (st + sl + 16) >> 5;
        else if (l) dc = (sl + 8) >> 4;
        else if (t) dc = (st + 8) >> 4;
        else dc = 128;
        for (int y = 0; y < 16; y++) memset(dst + y * stride, dc, 16);
        break;
    }
    default:
        plane_predict(dst, stride, 16, 5);
        break;
    }
}

void h264_intra_chroma(uint8_t *dst, ptrdiff_t stride, int mode, unsigned avail) {
    const uint8_t *above = dst - stride;
    switch (mode) {
    case 0: {
        const bool l = avail & AVAIL_LEFT, t = avail & AVAIL_TOP;
        for (int by = 0; by < 2; by++) {
            for (int bx = 0; bx < 2; bx++) {
                int st = 0, sl = 0;
                for (int i = 0; i < 4; i++) {
                    st += above[bx * 4 + i];
                    sl += dst[(by * 4 + i) * stride - 1];
                }
                int dc;
                if (bx == by) {
                    if (l && t) dc = (st + sl + 4) >> 3;
                    else if (l) dc = (sl + 2) >> 2;
                    else if (t) dc = (st + 2) >> 2;
                    else dc = 128;
                } else if (bx) {
                    if (t) dc = (st + 2) >> 2;
                    else if (l) dc = (sl + 2) >> 2;
                    else dc = 128;
                } else {
                    if (l) dc = (sl + 2) >> 2;
                    else if (t) dc = (st + 2) >> 2;
                    else dc = 128;
                }
                for (int y = 0; y < 4; y++) memset(dst + (by * 4 + y) * stride + bx * 4, dc, 4);
            }
        }
        break;
    }
    case 1:
        for (int y = 0; y < 8; y++) memset(dst + y * stride, dst[y * stride - 1], 8);
        break;
    case 2:
        for (int y = 0; y < 8; y++) memcpy(dst + y * stride, above, 8);
        break;
    default:
        plane_predict(dst, stride, 8, 34);
        break;
    }
}
