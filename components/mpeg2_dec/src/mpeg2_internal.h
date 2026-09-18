/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdatomic.h>
#include <string.h>
#include "mpeg2_dec.h"

#define MAX_HELD 4
#define MAX_POOL (3 + MAX_HELD + 1)
#define MAX_ROWS 80
#define MAX_SLICES 512
#define MAX_WORKERS 2
#define BITS_CHUNK 4096
#define BITS_RESERVE 1280
#define BITS_PADDING 8
#define ROW_MARGIN 16
#define MC_STRIDE 32
#define FRAME_SLACK 64

#define SC_PICTURE 0x00
#define SC_SLICE_FIRST 0x01
#define SC_SLICE_LAST 0xAF
#define SC_USER_DATA 0xB2
#define SC_SEQUENCE 0xB3
#define SC_EXTENSION 0xB5
#define SC_SEQUENCE_END 0xB7
#define SC_GOP 0xB8

#define EXT_SEQUENCE 1
#define EXT_DISPLAY 2
#define EXT_QUANT 3
#define EXT_SCALABLE 5
#define EXT_PICTURE_CODING 8
#define EXT_SPATIAL 9
#define EXT_TEMPORAL 10

#define MB_QUANT 1
#define MB_FWD 2
#define MB_BWD 4
#define MB_PATTERN 8
#define MB_INTRA 16

#define MBAI_ESCAPE 34
#define DCT_EOB 0x1000
#define DCT_ESCAPE 0x2000

static inline int clip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }
static inline uint8_t clip_u8(int v) { return (uint8_t)(v & ~0xFF ? (-v) >> 31 : v); }

typedef struct {
    const uint8_t *src;
    const uint8_t *src_end;
    uint8_t *buf;
    uint32_t len;
    uint32_t pos;
    bool exhausted;
} bits_t;

void mpeg2_bits_init(bits_t *b, uint8_t *buf, const uint8_t *data, size_t len);
void mpeg2_bits_refill(bits_t *b);

static inline void bits_reserve(bits_t *b) {
    if (!b->exhausted && b->len - (b->pos >> 3) < BITS_RESERVE) mpeg2_bits_refill(b);
}

static inline uint32_t bits_peek(const bits_t *b) {
    const uint8_t *p = b->buf + (b->pos >> 3);
    const unsigned s = b->pos & 7;
    const uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    return (v << s) | (uint32_t)(p[4] >> (8 - s));
}

static inline void bits_skip(bits_t *b, unsigned n) { b->pos += n; }

static inline uint32_t bits_u(bits_t *b, unsigned n) {
    const uint32_t v = bits_peek(b) >> (32 - n);
    b->pos += n;
    return v;
}

static inline uint32_t bits_u1(bits_t *b) {
    const uint32_t v = (b->buf[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}

static inline bool bits_overrun(const bits_t *b) { return b->pos > b->len * 8; }

typedef struct {
    const uint8_t *p;
    size_t len;
    size_t pos;
} hbits_t;

static inline uint32_t hbits_u(hbits_t *h, unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++, h->pos++) {
        const size_t byte = h->pos >> 3;
        const uint32_t bit = byte < h->len ? (h->p[byte] >> (7 - (h->pos & 7))) & 1 : 0;
        v = (v << 1) | bit;
    }
    return v;
}

static inline bool hbits_overrun(const hbits_t *h) { return h->pos > h->len * 8; }

#define VLC_SUB 1u
#define VLC_ERROR (-0x800000)

typedef struct {
    uint32_t *table;
    uint8_t root;
} vlc_t;

typedef struct {
    uint32_t code;
    uint8_t len;
    int16_t value;
} vlc_code_t;

static inline int vlc_get(bits_t *b, const vlc_t *vlc) {
    const uint32_t v = bits_peek(b);
    uint32_t e = vlc->table[v >> (32 - vlc->root)];
    if (e & VLC_SUB) {
        const unsigned sub = (e >> 1) & 31;
        e = vlc->table[(e >> 8) + ((v << vlc->root) >> (32 - sub))];
        if (!(e & 0x3E)) return VLC_ERROR;
        b->pos += vlc->root;
    } else if (!(e & 0x3E)) {
        return VLC_ERROR;
    }
    b->pos += (e >> 1) & 31;
    return (int32_t)e >> 8;
}

typedef struct {
    uint16_t width;
    uint16_t height;
    uint8_t aspect;
    uint8_t frame_rate_code;
    bool have_ext;
    bool progressive;
    uint8_t chroma_format;
    uint8_t profile_level;
    bool low_delay;
    uint8_t matrix;
    uint8_t intra_q[64];
    uint8_t inter_q[64];
} seq_t;

typedef struct {
    uint16_t tr;
    uint8_t type;
    uint8_t f_code[2][2];
    uint8_t dc_precision;
    uint8_t structure;
    bool frame_pred_frame_dct;
    bool concealment_mv;
    bool q_scale_type;
    bool intra_vlc;
    bool alternate_scan;
    bool have_ext;
} pic_t;

typedef struct {
    const uint8_t *data;
    uint32_t len;
    uint16_t row;
} slice_ref_t;

typedef struct frame {
    uint8_t *data;
    int64_t tag;
    uint32_t gop;
    uint16_t tr;
    uint8_t type;
    bool waiting;
    bool queued;
    bool concealed;
    atomic_uchar holds;
} frame_t;

#ifdef MPEG2_DEC_PROFILE
#define PROF_START(w) const uint32_t prof_start_ = (w)->dec->config.clock ? (w)->dec->config.clock() : 0
#define PROF_STOP(w, stage)                                                                     \
    if ((w)->dec->config.clock) (w)->prof[stage] += (uint32_t)((w)->dec->config.clock() - prof_start_)
#else
#define PROF_START(w) (void)0
#define PROF_STOP(w, stage) (void)0
#endif

typedef struct worker {
    struct mpeg2_dec *dec;
    bits_t bits;
    uint8_t *bits_buf;
    uint8_t *y;
    uint8_t *u;
    uint8_t *v;
    uint8_t *ref_y;
    uint8_t *ref_u;
    uint8_t *ref_v;
    uint8_t *pred_y[2];
    uint8_t *pred_u[2];
    uint8_t *pred_v[2];
    uint8_t *scratch;
    int16_t *blk;
    uint8_t *done;
    int dc_pred[3];
    int pmv[2][2][2];
    int qscale;
    uint8_t prev_flags;
    bool concealed;
#ifdef MPEG2_DEC_PROFILE
    uint64_t prof[MPEG2_PROF_COUNT];
#endif
} worker_t;

struct mpeg2_dec {
    mpeg2_dec_config_t config;
    const char *error;
    uint8_t *work_base;
    size_t work_used;
    size_t work_tables_end;

    vlc_t vlc_mbai;
    vlc_t vlc_mbtype[3];
    vlc_t vlc_cbp;
    vlc_t vlc_mv;
    vlc_t vlc_dc[2];
    vlc_t vlc_dct[2];

    seq_t seq;
    bool have_seq;
    seq_t layout;
    bool have_layout;
    uint8_t intra_q[64];
    uint8_t inter_q[64];
    pic_t pic;
    bool have_picture_header;
    mpeg2_dec_stream_info_t info;

    uint16_t mb_w;
    uint16_t mb_h;
    uint16_t width;
    uint16_t height;
    uint32_t packed_stride;
    size_t frame_bytes;
    uint32_t ystride;
    uint32_t cstride;

    uint8_t *frame_block;
    frame_t frames[MAX_POOL];
    uint8_t pool_size;
    frame_t *old_ref;
    frame_t *new_ref;
    frame_t *cur;
    const frame_t *pred_ref[2];
    const frame_t *conceal_ref;

    uint8_t outq[MAX_POOL];
    uint8_t outq_head;
    uint8_t outq_count;
    bool have_out;
    uint32_t out_gop;
    uint16_t out_tr;

    uint32_t gop;
    uint32_t gop_anchors;
    bool closed_gop;
    bool broken_link;
    bool need_keyframe;
    int64_t cur_tag;
    bool in_picture;
    bool pic_error;

    slice_ref_t slices[MAX_SLICES];
    uint16_t slice_count;
    uint16_t row_first[MAX_ROWS + 1];

    worker_t *workers[MAX_WORKERS];
    uint8_t worker_count;
    atomic_int next_row;
    atomic_bool unsupported;
    void *sem_work;
    void *sem_done;
    bool threaded;
    bool stopping;
};

extern const uint8_t mpeg2_zigzag[64];
extern const uint8_t mpeg2_alternate[64];
extern const uint8_t mpeg2_default_intra_q[64];
extern const uint8_t mpeg2_nonlinear_qscale[32];

extern const vlc_code_t mpeg2_mbai_codes[];
extern const vlc_code_t mpeg2_mbtype_i_codes[];
extern const vlc_code_t mpeg2_mbtype_p_codes[];
extern const vlc_code_t mpeg2_mbtype_b_codes[];
extern const vlc_code_t mpeg2_cbp_codes[];
extern const vlc_code_t mpeg2_mv_codes[];
extern const vlc_code_t mpeg2_dc_luma_codes[];
extern const vlc_code_t mpeg2_dc_chroma_codes[];
extern const vlc_code_t mpeg2_dct0_codes[];
extern const vlc_code_t mpeg2_dct1_codes[];
extern const uint16_t mpeg2_mbai_count;
extern const uint16_t mpeg2_mbtype_i_count;
extern const uint16_t mpeg2_mbtype_p_count;
extern const uint16_t mpeg2_mbtype_b_count;
extern const uint16_t mpeg2_cbp_count;
extern const uint16_t mpeg2_mv_count;
extern const uint16_t mpeg2_dc_luma_count;
extern const uint16_t mpeg2_dc_chroma_count;
extern const uint16_t mpeg2_dct0_count;
extern const uint16_t mpeg2_dct1_count;

void *mpeg2_work_alloc(struct mpeg2_dec *dec, size_t bytes);
bool mpeg2_build_vlcs(struct mpeg2_dec *dec);
bool mpeg2_vlc_build(struct mpeg2_dec *dec, vlc_t *vlc, const vlc_code_t *codes, uint16_t count,
                     uint8_t root, bool signed_levels);

bool mpeg2_parse_sequence(hbits_t *h, seq_t *seq);
bool mpeg2_parse_extension(struct mpeg2_dec *dec, hbits_t *h);
bool mpeg2_parse_sequence_extension(hbits_t *h, seq_t *seq);
bool mpeg2_parse_picture(hbits_t *h, pic_t *pic);

bool mpeg2_decode_row(worker_t *w, int row);

void mpeg2_idct_put(uint8_t *dst, ptrdiff_t stride, int16_t *blk, uint8_t rows, uint8_t rows_ac);
void mpeg2_idct_add(uint8_t *dst, ptrdiff_t stride, int16_t *blk, uint8_t rows, uint8_t rows_ac);

void mpeg2_predict(worker_t *w, int mb_x, int row, uint8_t flags, const int mv[2][2]);
void mpeg2_conceal(worker_t *w, int mb_x, int row);
