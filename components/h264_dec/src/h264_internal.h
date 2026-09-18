/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdatomic.h>
#include <string.h>
#include "h264_dec.h"

#define MAX_SPS 32
#define MAX_PPS 256
#define MAX_REFS 16
#define MAX_HELD 4
#define MAX_POOL (MAX_REFS + 1 + MAX_HELD)
#define MAX_MMCO 32
#define NO_PIC 0xFF

#define LUMA_MARGIN 32
#define CHROMA_MARGIN 16
#define MC_TMP_STRIDE 32
#define FRAME_SLACK 64
#define MIN_SLOTS 3
#define WINDOW_MARGIN 16
#define WINDOW_ROWS 64
#define WINDOW_MIRROR 22
#define WINDOW_PAD 32
#define MAX_SLOTS 6
#define QUEUE_SIZE 8
#define LUMA_ABOVE 4
#define CHROMA_ABOVE 2
#define BITS_PADDING 8
#define BITS_MB_RESERVE 1024
#define BITS_CHUNK 16384

#define NAL_SLICE 1
#define NAL_IDR 5
#define NAL_SEI 6
#define NAL_SPS 7
#define NAL_PPS 8

#define SLICE_P 0
#define SLICE_B 1
#define SLICE_I 2

typedef enum {
    MB_INTRA4x4 = 0,
    MB_INTRA16x16,
    MB_PCM,
    MB_INTER,
    MB_SKIP,
} mb_kind_t;

static inline int clip3(int lo, int hi, int v) { return v < lo ? lo : (v > hi ? hi : v); }
static inline uint8_t clip_u8(int v) { return (uint8_t)(v & ~0xFF ? (-v) >> 31 : v); }
static inline int iabs(int v) { return v < 0 ? -v : v; }
static inline int imin(int a, int b) { return a < b ? a : b; }
static inline int imax(int a, int b) { return a > b ? a : b; }

extern const uint8_t h264_clz8[256];

static inline int clz32(uint32_t v) {
    int n = 0;
    if (!(v & 0xFFFF0000u)) { n = 16; v <<= 16; }
    if (!(v & 0xFF000000u)) { n += 8; v <<= 8; }
    return n + h264_clz8[v >> 24];
}

typedef struct {
    const uint8_t *src;
    const uint8_t *src_end;
    uint8_t *buf;
    uint32_t cap;
    uint32_t len;
    uint32_t pos;
    uint32_t end_bits;
    uint8_t zeros;
    uint8_t stop_bits;
    bool exhausted;
} bits_t;

bool bits_init(bits_t *b, uint8_t *buf, uint32_t cap, const uint8_t *nal, size_t len);
void bits_refill(bits_t *b);

static inline void bits_reserve(bits_t *b, uint32_t bytes) {
    if (!b->exhausted && b->len - (b->pos >> 3) < bytes) bits_refill(b);
}

static inline uint32_t bits_peek(const bits_t *b) {
    const uint8_t *p = b->buf + (b->pos >> 3);
    const unsigned s = b->pos & 7;
    const uint32_t v = ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
    return (v << s) | (uint32_t)(p[4] >> (8 - s));
}

static inline void bits_skip(bits_t *b, unsigned n) { b->pos += n; }

static inline uint32_t bits_u(bits_t *b, unsigned n) {
    if (!n) return 0;
    const uint32_t v = bits_peek(b) >> (32 - n);
    b->pos += n;
    return v;
}

static inline uint32_t bits_u1(bits_t *b) {
    const uint32_t v = (b->buf[b->pos >> 3] >> (7 - (b->pos & 7))) & 1;
    b->pos++;
    return v;
}

static inline uint32_t bits_ue(bits_t *b) {
    const uint32_t v = bits_peek(b);
    const int lz = clz32(v);
    if (lz < 16) {
        b->pos += 2 * lz + 1;
        return (v >> (31 - 2 * lz)) - 1;
    }
    if (lz >= 32) {
        b->pos = b->len * 8 + 64;
        return 0;
    }
    b->pos += lz;
    const uint32_t tail = bits_u(b, lz + 1);
    return tail - 1;
}

static inline int32_t bits_se(bits_t *b) {
    const uint32_t k = bits_ue(b);
    return (k & 1) ? (int32_t)((k + 1) >> 1) : -(int32_t)(k >> 1);
}

static inline bool bits_overrun(const bits_t *b) {
    return b->pos > b->len * 8 || (b->exhausted && b->pos > b->end_bits);
}

static inline bool bits_past_buffer(const bits_t *b) {
    return b->pos > (b->len + BITS_PADDING) * 8;
}

static inline bool bits_more_data(const bits_t *b) {
    return !b->exhausted || b->pos < b->end_bits;
}

static inline bool bits_aligned(const bits_t *b) { return (b->pos & 7) == 0; }

#define CABAC_CONTEXTS 460

typedef struct {
    bits_t *bits;
    uint32_t range;
    uint32_t offset;
    uint8_t state[CABAC_CONTEXTS];
} cabac_t;

extern const int8_t h264_cabac_init_i[CABAC_CONTEXTS][2];
extern const int8_t h264_cabac_init_pb[3][CABAC_CONTEXTS][2];
extern const uint8_t h264_cabac_range_lps[64][4];
extern const uint8_t h264_cabac_trans_mps[64];
extern const uint8_t h264_cabac_trans_lps[64];
extern const uint8_t h264_sig_coeff_offset_8x8[63];
extern const uint8_t h264_last_coeff_offset_8x8[63];

void h264_cabac_init(cabac_t *c, bits_t *b, int qp, int model);
void h264_cabac_reinit(cabac_t *c);
int h264_cabac_decision(cabac_t *c, int ctx);
int h264_cabac_bypass(cabac_t *c);
int h264_cabac_terminate(cabac_t *c);
int h264_cabac_mb_type_i(cabac_t *c, int base, int inc, bool intra_slice);
int h264_cabac_mb_type_p(cabac_t *c);
int h264_cabac_mb_type_b(cabac_t *c, int inc);
int h264_cabac_sub_type_p(cabac_t *c);
int h264_cabac_sub_type_b(cabac_t *c);
int h264_cabac_ref(cabac_t *c, int inc);
int h264_cabac_mvd(cabac_t *c, int base, int amvd);
int h264_cabac_intra_pred_mode(cabac_t *c, int pred);
int h264_cabac_chroma_mode(cabac_t *c, int inc);
int h264_cabac_cbp_luma(cabac_t *c, int left, int top);
int h264_cabac_cbp_chroma(cabac_t *c, int left, int top);
int h264_cabac_qp_delta(cabac_t *c, bool prev_nonzero);
int h264_cabac_transform8x8(cabac_t *c, int inc);
int h264_cabac_cbf(cabac_t *c, int cat, int inc);
int h264_cabac_residual(cabac_t *c, int cat, int max_coeff, int16_t *levels, uint8_t *positions);

typedef struct {
    int16_t *table;
    uint8_t root_bits;
} vlc_t;

static inline int vlc_read(bits_t *b, const vlc_t *vlc) {
    const uint32_t v = bits_peek(b);
    int e = vlc->table[v >> (32 - vlc->root_bits)];
    if (e < 0) {
        const int base = -e;
        const uint8_t sub_bits = (uint8_t)vlc->table[base];
        e = vlc->table[base + 1 + ((v << vlc->root_bits) >> (32 - sub_bits))];
        if (e < 0) return -1;
        b->pos += vlc->root_bits;
    }
    b->pos += e & 0x1F;
    return e >> 5;
}

#define MAX_POC_CYCLE 256

typedef struct {
    bool valid;
    uint8_t profile_idc;
    uint8_t constraint_flags;
    uint8_t level_idc;
    uint8_t log2_max_frame_num;
    uint8_t poc_type;
    uint8_t log2_max_poc_lsb;
    bool delta_pic_order_always_zero;
    int32_t offset_for_non_ref_pic;
    int32_t offset_for_top_to_bottom;
    uint16_t poc_cycle_length;
    int32_t poc_cycle_sum;
    int32_t offset_for_ref_frame[MAX_POC_CYCLE];
    uint8_t max_num_ref_frames;
    bool gaps_allowed;
    bool direct_8x8_inference;
    bool has_bitstream_restriction;
    uint8_t num_reorder_frames;
    uint8_t max_dec_frame_buffering;
    uint16_t mb_width;
    uint16_t mb_height;
    uint16_t crop_left;
    uint16_t crop_right;
    uint16_t crop_top;
    uint16_t crop_bottom;
    bool full_range;
    uint8_t matrix;
    bool has_scaling;
    uint8_t scaling4[6][16];
    uint8_t scaling8[2][64];
    const char *unsupported;
} sps_t;

typedef struct {
    bool valid;
    bool cabac;
    uint8_t sps_id;
    bool bottom_field_pic_order_present;
    uint8_t num_ref_idx_default;
    uint8_t num_ref_idx_l1_default;
    bool weighted_pred;
    uint8_t weighted_bipred_idc;
    int8_t pic_init_qp;
    int8_t chroma_qp_offset[2];
    bool deblocking_control;
    bool constrained_intra_pred;
    bool redundant_pic_cnt_present;
    bool transform_8x8_mode;
    bool has_scaling;
    uint8_t scaling4[6][16];
    uint8_t scaling8[2][64];
    const char *unsupported;
} pps_t;

typedef struct {
    uint8_t op;
    uint32_t a;
    uint32_t b;
} mmco_t;

#define MAX_REORDER 33

typedef struct {
    int16_t weight;
    int16_t offset;
} weight_t;

typedef struct {
    uint32_t first_mb;
    uint8_t type;
    uint8_t nal_ref_idc;
    bool idr;
    uint8_t pps_id;
    uint16_t frame_num;
    uint32_t poc_lsb;
    int32_t delta_poc_bottom;
    int32_t delta_poc[2];
    bool no_output_of_prior_pics;
    uint32_t redundant_pic_cnt;
    uint8_t num_ref_idx_active;
    uint8_t num_ref_idx_l1;
    bool direct_spatial;
    uint8_t cabac_init_idc;
    int8_t qp;
    uint8_t disable_deblock;
    int8_t alpha_offset;
    int8_t beta_offset;
    uint8_t reorder_count[2];
    uint8_t reorder_idc[2][MAX_REORDER];
    uint32_t reorder_value[2][MAX_REORDER];
    bool weighted;
    uint8_t luma_denom;
    uint8_t chroma_denom;
    weight_t weights[2][MAX_REFS][3];
    bool long_term_reference;
    bool adaptive_marking;
    uint8_t mmco_count;
    mmco_t mmco[MAX_MMCO];
} slice_t;

typedef struct {
    int8_t ref[4];
    uint8_t refpic[4];
    int16_t mv[16][2];
    uint8_t mvd_right[4][2];
    uint8_t mvd_bottom[4][2];
} mbmotion_t;

typedef struct {
    uint16_t slice;
    uint16_t nzmask;
    uint8_t uniform;
    uint8_t kind;
    uint8_t t8x8;
    int8_t qp;
    uint8_t filter;
    int8_t alpha;
    int8_t beta;
    uint8_t cqp[2];
    uint8_t cflags;
    uint8_t direct8;
    uint16_t cbf;
    int8_t modes[16];
    uint8_t nnz[24];
    mbmotion_t m[2];
} mbinfo_t;

typedef struct {
    uint8_t *y;
    uint8_t *u;
    uint8_t *v;
    uint8_t *mb;
} rowbuf_t;

static inline mbinfo_t *mb_at(const rowbuf_t *row, uint32_t x, uint32_t stride) {
    return (mbinfo_t *)(row->mb + (size_t)x * stride);
}

#define JOB_ROW 0
#define JOB_STOP 1

typedef struct {
    int16_t mv[2];
    int8_t ref;
    uint8_t pic;
} colblk_t;

typedef struct {
    uint8_t *frame;
    uint8_t *col;
    uint16_t mb_y;
    int8_t slot;
    int8_t prev;
    uint8_t kind;
    bool last;
} row_job_t;

typedef struct {
    row_job_t jobs[QUEUE_SIZE];
    atomic_uint head;
    atomic_uint tail;
} job_queue_t;

typedef struct {
    int8_t slots[QUEUE_SIZE];
    atomic_uint head;
    atomic_uint tail;
} slot_queue_t;

typedef struct frame {
    uint8_t *data;
    uint8_t *col;
    int32_t frame_num_wrap;
    int32_t poc;
    int64_t tag;
    uint16_t frame_num;
    uint8_t long_term_idx;
    uint8_t ref;
    bool non_existing;
    bool concealed;
    bool needed_for_output;
    uint32_t epoch;
    atomic_uchar holds;
} frame_t;

#define MB_CF_CHROMA 1
#define MB_CF_DIRECT 2

#define REF_NONE 0
#define REF_SHORT 1
#define REF_LONG 2

#ifdef H264_DEC_PROFILE
#define PROF_START(dec) const uint32_t prof_start_ = (dec)->config.clock ? (dec)->config.clock() : 0
#define PROF_STOP(dec, stage)                                                                   \
    if ((dec)->config.clock) (dec)->prof[stage] += (uint32_t)((dec)->config.clock() - prof_start_)
#else
#define PROF_START(dec) (void)0
#define PROF_STOP(dec, stage) (void)0
#endif

struct h264_dec {
    h264_dec_config_t config;
#ifdef H264_DEC_PROFILE
    uint64_t prof[H264_PROF_COUNT];
#endif
    const char *error;

    sps_t sps[MAX_SPS];
    pps_t pps[MAX_PPS];
    sps_t active;
    bool have_active;
    uint8_t active_sps_id;
    h264_dec_stream_info_t info;

    uint16_t mb_w;
    uint16_t mb_h;
    uint16_t mb_stride;
    bool has_l1;
    uint16_t width;
    uint16_t height;
    uint32_t packed_stride;
    size_t frame_bytes;
    uint32_t luma_stride;
    uint32_t chroma_stride;

    uint8_t *frame_block;
    frame_t frames[MAX_POOL];
    uint8_t pool_size;
    uint8_t dpb_size;
    uint8_t reorder_frames;
    uint8_t max_long_term_plus1;
    uint16_t prev_ref_frame_num;
    bool need_keyframe;

    int32_t prev_poc_msb;
    int32_t prev_poc_lsb;
    int32_t prev_frame_num_offset;
    uint16_t prev_frame_num;
    int32_t cur_poc;
    int32_t cur_poc_top;
    bool cur_mmco5;
    bool skip_leading;
    int32_t leading_poc;
    int64_t cur_tag;
    uint32_t epoch;
    bool draining;
    bool have_out_poc;
    uint32_t out_epoch;
    int32_t out_poc;
    int32_t poc_step;

    frame_t *cur;
    const pps_t *cur_pps;
    bool in_picture;
    bool pic_error;
    uint32_t next_mb;
    uint16_t slice_num;
    slice_t first_slice;
    slice_t slice;
    frame_t *last_ref;

    frame_t *ref_list[2][MAX_REFS + 1];
    uint8_t ref_count[2];
    int16_t implicit_w[MAX_REFS][MAX_REFS];
    uint8_t *col_block;
    size_t col_bytes;
    uint8_t col_blocks;

    bits_t bits;
    cabac_t *cabac;
    bool cabac_on;
    bool last_qp_delta_nonzero;
    uint8_t *bits_buf;
    uint32_t bits_cap;

    rowbuf_t rows[MAX_SLOTS];
    uint8_t slot_count;
    int8_t cur_slot;
    int8_t above_slot;
    uint8_t *win_y;
    uint8_t *win_u;
    uint8_t *win_v;
    const frame_t *win_frame;
    bool win_disabled;
    int win_target;
    atomic_int win_claimed;
    atomic_int win_busy;
    atomic_int win_floor;
    atomic_int win_goal;
    const frame_t *_Atomic win_source;
    void *sem_window;
    uint32_t win_stride;
    uint32_t win_cstride;
    int win_lo;
    int win_hi;
    uint8_t *top_y;
    uint8_t *top_u;
    uint8_t *top_v;
    uint8_t *packed_row;
    job_queue_t work;
    slot_queue_t free_slots;
    void *sem_work;
    void *sem_free;
    void *sem_done;
    bool threaded;
    uint8_t *mc_src;
    uint8_t *mc_a;
    uint8_t *mc_b;
    uint8_t *mc_chroma;
    uint8_t *bi_y[2];
    uint8_t *bi_u[2];
    uint8_t *bi_v[2];
    uint8_t *mc_scratch;
    int16_t *mc_mid;
    int16_t *coeff;
    int16_t *dc;

    vlc_t coeff_token[4];
    vlc_t coeff_token_dc;
    vlc_t total_zeros[15];
    vlc_t total_zeros_dc[3];
    vlc_t run_before[7];

    int16_t *ls4;
    int16_t *ls8;
    const pps_t *ls_pps;

    uint8_t *work_base;
    size_t work_used;
    size_t work_tables_end;
};

void *h264_work_alloc(struct h264_dec *dec, size_t bytes);
bool h264_build_vlcs(struct h264_dec *dec);

bool h264_parse_sps(struct h264_dec *dec, bits_t *b);
bool h264_parse_pps(struct h264_dec *dec, bits_t *b);
bool h264_parse_sps_standalone(bits_t *b, sps_t *out, uint8_t *id);

int h264_parse_slice_header(struct h264_dec *dec, bits_t *b, uint8_t nal_type, uint8_t nal_ref_idc,
                            slice_t *s);
bool h264_build_ref_list(struct h264_dec *dec, const slice_t *s);
void h264_build_implicit_weights(struct h264_dec *dec);
frame_t *h264_free_frame(struct h264_dec *dec);
int h264_gap_frames_left(const struct h264_dec *dec);
void h264_mark_references(struct h264_dec *dec);
void h264_handle_frame_num_gap(struct h264_dec *dec, uint16_t frame_num);
int32_t h264_compute_poc(struct h264_dec *dec, const slice_t *s, bool commit);
void h264_discard_waiting(struct h264_dec *dec);

bool h264_decode_slice_data(struct h264_dec *dec);
void h264_conceal_mb(struct h264_dec *dec, uint32_t mb_addr);

void h264_row_done(struct h264_dec *dec, uint32_t mb_y);
void h264_rows_begin_picture(struct h264_dec *dec);
void h264_rows_reset(struct h264_dec *dec);
bool h264_rows_start(struct h264_dec *dec);
void h264_rows_stop(struct h264_dec *dec);

#define AVAIL_LEFT 1
#define AVAIL_TOP 2
#define AVAIL_TOP_RIGHT 4
#define AVAIL_TOP_LEFT 8

void h264_intra4x4(uint8_t *dst, ptrdiff_t stride, int mode, unsigned avail);
void h264_intra16x16(uint8_t *dst, ptrdiff_t stride, int mode, unsigned avail);
void h264_intra_chroma(uint8_t *dst, ptrdiff_t stride, int mode, unsigned avail);

void h264_idct4x4_add(uint8_t *dst, ptrdiff_t stride, int16_t *block);
void h264_idct8x8_add(uint8_t *dst, ptrdiff_t stride, const int16_t *block);
void h264_luma_dc_dequant(int16_t *dc, int qp, int scale);
void h264_chroma_dc_dequant(int16_t *dc, int qp, int scale);

void h264_intra8x8(uint8_t *dst, ptrdiff_t stride, int mode, unsigned avail);

void h264_weight_block(uint8_t *dst, ptrdiff_t stride, int h, int weight, int offset, int denom);
void h264_avg_block(uint8_t *dst, ptrdiff_t stride, const uint8_t *a, const uint8_t *b,
                    ptrdiff_t src_stride, int h);
void h264_weight_bi_block(uint8_t *dst, ptrdiff_t stride, const uint8_t *a, const uint8_t *b,
                          ptrdiff_t src_stride, int w, int h, int w0, int w1, int offset,
                          int denom);
void h264_weight_bi_implicit(uint8_t *dst, ptrdiff_t stride, const uint8_t *a, const uint8_t *b,
                             ptrdiff_t src_stride, int h, int w1);

void h264_mc_luma(struct h264_dec *dec, const frame_t *ref, uint8_t *dst, ptrdiff_t stride,
                  int x, int y, int w, int h, int mvx, int mvy);
void h264_mc_chroma(struct h264_dec *dec, const frame_t *ref, uint8_t *dst_u, uint8_t *dst_v,
                    ptrdiff_t stride, int x, int y, int w, int h, int mvx, int mvy);

void h264_window_advance(struct h264_dec *dec, uint32_t mb_y);
void h264_window_fill(struct h264_dec *dec);
bool h264_window_fill_one(struct h264_dec *dec, int limit, atomic_int *busy);
void h264_window_request(struct h264_dec *dec, int target);
void h264_window_wait(struct h264_dec *dec, int rows);

static inline const uint8_t *h264_window_luma(const struct h264_dec *dec, const frame_t *f, int y,
                                             int rows) {
    if (f != dec->win_frame || y < dec->win_lo || y + rows > dec->win_hi) return NULL;
    return dec->win_y + (size_t)(y & (WINDOW_ROWS - 1)) * dec->win_stride;
}

static inline const uint8_t *h264_window_chroma(const struct h264_dec *dec, const frame_t *f,
                                               int comp, int y, int rows) {
    if (f != dec->win_frame || 2 * y < dec->win_lo || 2 * (y + rows) > dec->win_hi) return NULL;
    return (comp ? dec->win_v : dec->win_u) +
           (size_t)(y & (WINDOW_ROWS / 2 - 1)) * dec->win_cstride;
}

void h264_deblock_row(struct h264_dec *dec, rowbuf_t *cur, rowbuf_t *above, uint32_t mb_y);

void h264_store_col_row(struct h264_dec *dec, const rowbuf_t *row, uint8_t *col, uint32_t mb_y);

void h264_pack_rows(uint8_t *dst, uint32_t dst_stride, const uint8_t *y, uint32_t y_stride,
                    const uint8_t *u, const uint8_t *v, uint32_t c_stride, uint32_t width,
                    uint32_t rows);

extern const uint8_t h264_zigzag4x4[16];
extern const uint8_t h264_zigzag8x8[64];
extern const uint8_t h264_col_corner[4];
extern const uint8_t h264_dequant8[6][64];
extern const uint8_t h264_default_scaling4[2][16];
extern const uint8_t h264_default_scaling8[2][64];

void h264_build_level_scales(struct h264_dec *dec);
extern const uint8_t h264_chroma_qp[52];
extern const uint8_t h264_dequant4[6][16];
