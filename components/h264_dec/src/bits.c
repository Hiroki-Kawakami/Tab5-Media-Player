/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_internal.h"

bool bits_init(bits_t *b, uint8_t *buf, uint32_t cap, const uint8_t *nal, size_t len) {
    while (len && nal[len - 1] == 0) len--;
    if (!len) return false;
    uint8_t last = nal[len - 1];
    uint8_t stop = 1;
    while (!(last & 1)) {
        last >>= 1;
        stop++;
    }
    b->src = nal;
    b->src_end = nal + len;
    b->buf = buf;
    b->cap = cap;
    b->len = 0;
    b->pos = 0;
    b->end_bits = 0;
    b->zeros = 0;
    b->stop_bits = stop;
    b->exhausted = false;
    bits_refill(b);
    return true;
}

void bits_refill(bits_t *b) {
    if (b->exhausted) return;
    const uint32_t consumed = b->pos >> 3;
    const uint32_t keep = b->len > consumed ? b->len - consumed : 0;
    if (consumed && keep) memmove(b->buf, b->buf + consumed, keep);
    b->pos -= consumed * 8;
    uint8_t *out = b->buf + keep;
    const uint8_t *limit = b->buf + b->cap;
    const uint8_t *src = b->src;
    const uint8_t *end = b->src_end;
    uint8_t zeros = b->zeros;
    while (src < end && out < limit) {
        const uint8_t v = *src++;
        if (zeros >= 2 && v == 3) {
            zeros = 0;
            continue;
        }
        *out++ = v;
        zeros = v ? 0 : (uint8_t)(zeros + 1);
    }
    b->src = src;
    b->zeros = zeros;
    b->len = (uint32_t)(out - b->buf);
    if (src >= end) {
        b->exhausted = true;
        b->end_bits = b->len * 8 - b->stop_bits;
    }
    memset(out, 0, BITS_PADDING);
}
