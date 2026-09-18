/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "mpeg2_internal.h"

void mpeg2_bits_init(bits_t *b, uint8_t *buf, const uint8_t *data, size_t len) {
    const size_t n = len < BITS_CHUNK ? len : BITS_CHUNK;
    memcpy(buf, data, n);
    memset(buf + n, 0, BITS_PADDING);
    b->src = data + n;
    b->src_end = data + len;
    b->buf = buf;
    b->len = (uint32_t)n;
    b->pos = 0;
    b->exhausted = n == len;
}

void mpeg2_bits_refill(bits_t *b) {
    const uint32_t byte = b->pos >> 3;
    const uint32_t keep = b->len > byte ? b->len - byte : 0;
    memmove(b->buf, b->buf + byte, keep);
    size_t room = BITS_CHUNK - keep;
    const size_t left = (size_t)(b->src_end - b->src);
    if (room > left) room = left;
    memcpy(b->buf + keep, b->src, room);
    b->src += room;
    b->len = keep + (uint32_t)room;
    b->pos &= 7;
    b->exhausted = b->src == b->src_end;
    memset(b->buf + b->len, 0, BITS_PADDING);
}

static void load_matrix(hbits_t *h, uint8_t *q) {
    for (int i = 0; i < 64; i++) q[mpeg2_zigzag[i]] = (uint8_t)hbits_u(h, 8);
}

bool mpeg2_parse_sequence(hbits_t *h, seq_t *seq) {
    memset(seq, 0, sizeof(*seq));
    seq->width = (uint16_t)hbits_u(h, 12);
    seq->height = (uint16_t)hbits_u(h, 12);
    seq->aspect = (uint8_t)hbits_u(h, 4);
    seq->frame_rate_code = (uint8_t)hbits_u(h, 4);
    hbits_u(h, 18);
    hbits_u(h, 1);
    hbits_u(h, 10);
    hbits_u(h, 1);
    if (hbits_u(h, 1)) {
        load_matrix(h, seq->intra_q);
    } else {
        memcpy(seq->intra_q, mpeg2_default_intra_q, 64);
    }
    if (hbits_u(h, 1)) {
        load_matrix(h, seq->inter_q);
    } else {
        memset(seq->inter_q, 16, 64);
    }
    seq->progressive = true;
    seq->chroma_format = 1;
    return !hbits_overrun(h) && seq->width && seq->height;
}

bool mpeg2_parse_sequence_extension(hbits_t *h, seq_t *seq) {
    seq->profile_level = (uint8_t)hbits_u(h, 8);
    seq->progressive = hbits_u(h, 1);
    seq->chroma_format = (uint8_t)hbits_u(h, 2);
    seq->width = (uint16_t)(seq->width | (hbits_u(h, 2) << 12));
    seq->height = (uint16_t)(seq->height | (hbits_u(h, 2) << 12));
    hbits_u(h, 12);
    hbits_u(h, 1);
    hbits_u(h, 8);
    seq->low_delay = hbits_u(h, 1);
    hbits_u(h, 7);
    seq->have_ext = true;
    return !hbits_overrun(h);
}

static void parse_display_extension(hbits_t *h, seq_t *seq) {
    hbits_u(h, 3);
    if (hbits_u(h, 1)) {
        hbits_u(h, 8);
        hbits_u(h, 8);
        seq->matrix = (uint8_t)hbits_u(h, 8);
    }
}

static bool parse_picture_coding(hbits_t *h, pic_t *pic) {
    pic->f_code[0][0] = (uint8_t)hbits_u(h, 4);
    pic->f_code[0][1] = (uint8_t)hbits_u(h, 4);
    pic->f_code[1][0] = (uint8_t)hbits_u(h, 4);
    pic->f_code[1][1] = (uint8_t)hbits_u(h, 4);
    pic->dc_precision = (uint8_t)hbits_u(h, 2);
    pic->structure = (uint8_t)hbits_u(h, 2);
    hbits_u(h, 1);
    pic->frame_pred_frame_dct = hbits_u(h, 1);
    pic->concealment_mv = hbits_u(h, 1);
    pic->q_scale_type = hbits_u(h, 1);
    pic->intra_vlc = hbits_u(h, 1);
    pic->alternate_scan = hbits_u(h, 1);
    pic->have_ext = true;
    return !hbits_overrun(h);
}

bool mpeg2_parse_extension(struct mpeg2_dec *dec, hbits_t *h) {
    const uint32_t id = hbits_u(h, 4);
    switch (id) {
    case EXT_SEQUENCE:
        if (!dec->have_seq) return true;
        return mpeg2_parse_sequence_extension(h, &dec->seq);
    case EXT_DISPLAY:
        if (dec->have_seq) parse_display_extension(h, &dec->seq);
        return true;
    case EXT_QUANT:
        if (hbits_u(h, 1)) load_matrix(h, dec->intra_q);
        if (hbits_u(h, 1)) load_matrix(h, dec->inter_q);
        return !hbits_overrun(h);
    case EXT_PICTURE_CODING:
        if (!dec->have_picture_header) return true;
        return parse_picture_coding(h, &dec->pic);
    case EXT_SCALABLE:
    case EXT_SPATIAL:
    case EXT_TEMPORAL:
        dec->error = "scalable MPEG-2 is not supported";
        return false;
    default:
        return true;
    }
}

bool mpeg2_parse_picture(hbits_t *h, pic_t *pic) {
    memset(pic, 0, sizeof(*pic));
    pic->tr = (uint16_t)hbits_u(h, 10);
    pic->type = (uint8_t)hbits_u(h, 3);
    hbits_u(h, 16);
    return !hbits_overrun(h) && pic->type >= MPEG2_PICTURE_I && pic->type <= MPEG2_PICTURE_B;
}
