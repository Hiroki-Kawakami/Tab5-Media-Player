/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include <stddef.h>
#include "h264_internal.h"
#include "kernels.h"

typedef struct {
    const uint8_t *data;
    size_t len;
} nal_t;

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
    uint8_t length_size;
} nal_iter_t;

static const uint8_t *find_start_code(const uint8_t *p, const uint8_t *end) {
    while (p + 3 <= end) {
        const uint8_t *z = memchr(p, 0, (size_t)(end - p - 2));
        if (!z) return end;
        if (z[1] == 0 && z[2] == 1) return z;
        p = z + 1;
    }
    return end;
}

static void nal_iter_init(nal_iter_t *it, const uint8_t *data, size_t len, uint8_t length_size) {
    it->p = data;
    it->end = data + len;
    it->length_size = length_size;
    if (!length_size) {
        const uint8_t *sc = find_start_code(it->p, it->end);
        it->p = sc < it->end ? sc + 3 : it->end;
    }
}

static bool nal_next(nal_iter_t *it, nal_t *nal) {
    if (it->length_size) {
        while (it->p + it->length_size <= it->end) {
            uint32_t n = 0;
            for (uint8_t i = 0; i < it->length_size; i++) n = (n << 8) | it->p[i];
            it->p += it->length_size;
            if (n > (size_t)(it->end - it->p)) {
                it->p = it->end;
                return false;
            }
            nal->data = it->p;
            nal->len = n;
            it->p += n;
            if (n) return true;
        }
        return false;
    }
    while (it->p < it->end) {
        const uint8_t *next = find_start_code(it->p, it->end);
        nal->data = it->p;
        nal->len = (size_t)(next - it->p);
        it->p = next < it->end ? next + 3 : it->end;
        if (nal->len) return true;
    }
    return false;
}

void *h264_work_alloc(struct h264_dec *dec, size_t bytes) {
    const size_t align = H264_DEC_WORK_ALIGNMENT;
    const size_t start = (dec->work_used + align - 1) & ~(align - 1);
    if (start + bytes > dec->config.work_bytes) return NULL;
    dec->work_used = start + bytes;
    return dec->work_base + start;
}

static void release_frames(h264_dec_t *dec) {
    if (dec->frame_block) dec->config.free(dec->config.ctx, dec->frame_block);
    dec->frame_block = NULL;
    if (dec->col_block) dec->config.free(dec->config.ctx, dec->col_block);
    dec->col_block = NULL;
    memset(dec->frames, 0, sizeof(dec->frames));
    dec->pool_size = 0;
    dec->cur = NULL;
    dec->last_ref = NULL;
    dec->in_picture = false;
}

static bool allocate_window(h264_dec_t *dec);

static bool setup_rows(h264_dec_t *dec) {
    dec->work_used = dec->work_tables_end;
    const uint32_t w = dec->width;
    dec->luma_stride = w + 2 * LUMA_MARGIN;
    dec->chroma_stride = w / 2 + 2 * CHROMA_MARGIN;
    dec->packed_row = h264_work_alloc(dec, (size_t)dec->packed_stride * 16 + 48);
    uint8_t *src = h264_work_alloc(dec, 1 + (21 + 1) * MC_TMP_STRIDE);
    dec->mc_src = src ? src + 1 : NULL;
    dec->mc_a = h264_work_alloc(dec, 16 * 16 + 16);
    dec->mc_b = h264_work_alloc(dec, 16 * 16 + 16);
    dec->mc_chroma = h264_work_alloc(dec, 16 * 10);
    for (int i = 0; i < 2; i++) {
        dec->bi_y[i] = h264_work_alloc(dec, 16 * 16 + 16);
        dec->bi_u[i] = h264_work_alloc(dec, 16 * 8 + 16);
        dec->bi_v[i] = h264_work_alloc(dec, 16 * 8 + 16);
        if (!dec->bi_y[i] || !dec->bi_u[i] || !dec->bi_v[i]) return false;
    }
    dec->mc_scratch = h264_work_alloc(dec, 64);
    if (dec->mc_scratch) memset(dec->mc_scratch, 0, 64);
    dec->mc_mid = h264_work_alloc(dec, sizeof(int16_t) * 16 * 22);
    dec->coeff = h264_work_alloc(dec, sizeof(int16_t) * 16 * 24);
    dec->dc = h264_work_alloc(dec, sizeof(int16_t) * 16 * 3);
    dec->cabac = h264_work_alloc(dec, sizeof(cabac_t));
    if (!dec->cabac) return false;
    dec->ls4 = h264_work_alloc(dec, sizeof(int16_t) * 6 * 6 * 16);
    dec->ls8 = h264_work_alloc(dec, sizeof(int16_t) * 2 * 6 * 64);
    dec->ls_pps = NULL;
    dec->top_y = h264_work_alloc(dec, w);
    dec->top_u = h264_work_alloc(dec, w / 2);
    dec->top_v = h264_work_alloc(dec, w / 2);
    if (!dec->packed_row || !dec->mc_src || !dec->mc_a || !dec->mc_b || !dec->mc_chroma ||
        !dec->mc_scratch || !dec->mc_mid || !dec->coeff || !dec->dc || !dec->ls4 || !dec->ls8 ||
        !dec->top_y || !dec->top_u || !dec->top_v) {
        return false;
    }
    dec->win_y = dec->win_u = dec->win_v = NULL;
    dec->win_frame = NULL;
    if (!allocate_window(dec)) dec->win_y = dec->win_u = dec->win_v = NULL;

    const size_t luma_bytes = (size_t)dec->luma_stride * (16 + LUMA_ABOVE);
    const size_t chroma_bytes = (size_t)dec->chroma_stride * (8 + CHROMA_ABOVE);
    const size_t mb_bytes = (size_t)dec->mb_stride * dec->mb_w;
    const size_t align = H264_DEC_WORK_ALIGNMENT * 4;
    const size_t slot_bytes = luma_bytes + 2 * chroma_bytes + mb_bytes + align;
    size_t slots = (dec->config.work_bytes - dec->work_used) / slot_bytes;
    if (slots > MAX_SLOTS) slots = MAX_SLOTS;
    if (slots < MIN_SLOTS) return false;
    for (size_t i = 0; i < slots; i++) {
        uint8_t *y = h264_work_alloc(dec, luma_bytes);
        uint8_t *u = h264_work_alloc(dec, chroma_bytes);
        uint8_t *v = h264_work_alloc(dec, chroma_bytes);
        uint8_t *mb = h264_work_alloc(dec, mb_bytes);
        if (!y || !u || !v || !mb) return false;
        dec->rows[i].y = y + dec->luma_stride * LUMA_ABOVE + LUMA_MARGIN;
        dec->rows[i].u = u + dec->chroma_stride * CHROMA_ABOVE + CHROMA_MARGIN;
        dec->rows[i].v = v + dec->chroma_stride * CHROMA_ABOVE + CHROMA_MARGIN;
        dec->rows[i].mb = mb;
    }
    dec->slot_count = (uint8_t)slots;
    h264_rows_reset(dec);
    return true;
}

static bool allocate_window(h264_dec_t *dec) {
    dec->win_stride = dec->width + WINDOW_PAD;
    dec->win_cstride = dec->width / 2 + WINDOW_PAD;
    const size_t luma = (size_t)(WINDOW_ROWS + WINDOW_MIRROR) * dec->win_stride + FRAME_SLACK;
    const size_t chroma = (size_t)(WINDOW_ROWS + WINDOW_MIRROR) / 2 * dec->win_cstride + FRAME_SLACK;
    const size_t bytes = luma + 2 * chroma + 3 * H264_DEC_WORK_ALIGNMENT;
    const size_t luma_bytes = (size_t)dec->luma_stride * (16 + LUMA_ABOVE);
    const size_t chroma_bytes = (size_t)dec->chroma_stride * (8 + CHROMA_ABOVE);
    const size_t slot_bytes = luma_bytes + 2 * chroma_bytes +
                              (size_t)dec->mb_stride * dec->mb_w + H264_DEC_WORK_ALIGNMENT * 4;
    const size_t left = dec->config.work_bytes - dec->work_used;
    if (left < bytes + MIN_SLOTS * slot_bytes + H264_DEC_WORK_ALIGNMENT) return false;
    dec->win_y = h264_work_alloc(dec, luma);
    dec->win_u = h264_work_alloc(dec, chroma);
    dec->win_v = h264_work_alloc(dec, chroma);
    return dec->win_y && dec->win_u && dec->win_v;
}

static uint32_t max_dpb_mbs(const sps_t *sps) {
    switch (sps->level_idc) {
    case 9: return 396;
    case 10: return 396;
    case 11: return (sps->constraint_flags & 0x10) ? 396 : 900;
    case 12: case 13: case 20: return 2376;
    case 21: return 4752;
    case 22: case 30: return 8100;
    case 31: return 18000;
    case 32: return 20480;
    case 40: case 41: return 32768;
    case 42: return 34816;
    case 50: return 110400;
    case 51: case 52: return 184320;
    default: return 696320;
    }
}

static uint8_t max_dpb_frames(const sps_t *sps, uint32_t mbs) {
    const uint32_t frames = mbs ? max_dpb_mbs(sps) / mbs : MAX_REFS;
    if (frames < 1) return 1;
    return frames > MAX_REFS ? MAX_REFS : (uint8_t)frames;
}

static bool same_layout(const sps_t *a, const sps_t *b) {
    return a->mb_width == b->mb_width && a->mb_height == b->mb_height &&
           a->max_num_ref_frames == b->max_num_ref_frames && a->level_idc == b->level_idc &&
           a->profile_idc == b->profile_idc && a->constraint_flags == b->constraint_flags &&
           a->has_bitstream_restriction == b->has_bitstream_restriction &&
           a->num_reorder_frames == b->num_reorder_frames &&
           a->max_dec_frame_buffering == b->max_dec_frame_buffering &&
           a->direct_8x8_inference == b->direct_8x8_inference && a->poc_type == b->poc_type &&
           a->crop_left == b->crop_left && a->crop_right == b->crop_right &&
           a->crop_top == b->crop_top && a->crop_bottom == b->crop_bottom &&
           a->full_range == b->full_range && a->matrix == b->matrix;
}

static bool frames_held(const h264_dec_t *dec) {
    for (uint8_t i = 0; i < dec->pool_size; i++) {
        if (atomic_load(&dec->frames[i].holds)) return true;
    }
    return false;
}

static bool activate_sps(h264_dec_t *dec, uint8_t sps_id) {
    const sps_t *sps = &dec->sps[sps_id];
    if (!sps->valid) {
        dec->error = "slice refers to a missing SPS";
        return false;
    }
    if (sps->unsupported) {
        dec->error = sps->unsupported;
        return false;
    }
    if (dec->have_active && dec->pool_size && same_layout(&dec->active, sps)) {
        dec->active = *sps;
        dec->active_sps_id = sps_id;
        return true;
    }

    const uint32_t mbs = (uint32_t)sps->mb_width * sps->mb_height;
    const uint32_t w = sps->mb_width * 16u;
    const uint32_t h = sps->mb_height * 16u;
    if ((dec->config.max_mbs && mbs > dec->config.max_mbs) ||
        (dec->config.max_side && (w > dec->config.max_side || h > dec->config.max_side))) {
        dec->error = "video is larger than this player supports";
        return false;
    }

    if (frames_held(dec)) {
        dec->error = "video format changed mid-stream";
        return false;
    }
    release_frames(dec);
    dec->have_active = false;
    dec->active = *sps;
    dec->active_sps_id = sps_id;
    dec->mb_w = sps->mb_width;
    dec->mb_h = sps->mb_height;
    dec->has_l1 = !(sps->profile_idc == 66 || (sps->constraint_flags & 0x80));
    dec->mb_stride = (uint16_t)(dec->has_l1 ? sizeof(mbinfo_t)
                                            : offsetof(mbinfo_t, m) + sizeof(mbmotion_t));
    dec->width = (uint16_t)w;
    dec->height = (uint16_t)h;
    dec->packed_stride = w * 3 / 2;
    dec->frame_bytes = (size_t)dec->packed_stride * h;

    if (!setup_rows(dec)) {
        dec->error = "video is too wide for the decoder's work memory";
        return false;
    }

    uint8_t held = dec->config.held_pictures;
    if (held > MAX_HELD) held = MAX_HELD;
    const uint8_t refs = sps->max_num_ref_frames ? sps->max_num_ref_frames : 1;
    uint8_t dpb = sps->has_bitstream_restriction ? sps->max_dec_frame_buffering
                                                 : max_dpb_frames(sps, mbs);
    if (dpb < refs) dpb = refs;
    if (dpb > MAX_REFS) dpb = MAX_REFS;
    uint8_t reorder = sps->has_bitstream_restriction ? sps->num_reorder_frames
                                                     : (sps->poc_type == 2 ? 0 : dpb);
    if (reorder > dpb) reorder = dpb;
    if (dec->config.frame_budget_bytes) {
        size_t room = dec->config.frame_budget_bytes / dec->frame_bytes;
        if (room < (size_t)(refs + 1 + held)) room = (size_t)refs + 1 + held;
        if ((size_t)dpb + 1 + held > room) {
            dpb = (uint8_t)(room - 1 - held);
            if (reorder > dpb) reorder = dpb;
        }
    }
    dec->dpb_size = dpb;
    dec->reorder_frames = reorder;
    dec->pool_size = (uint8_t)(dpb + 1 + held);
    dec->frame_block =
        dec->config.alloc(dec->config.ctx, dec->frame_bytes * dec->pool_size + FRAME_SLACK);
    if (!dec->frame_block) {
        dec->pool_size = 0;
        dec->error = "not enough memory for the reference frames";
        return false;
    }
    for (uint8_t i = 0; i < dec->pool_size; i++) {
        dec->frames[i].data = dec->frame_block + dec->frame_bytes * i;
    }
    dec->col_blocks = dec->has_l1 ? (sps->direct_8x8_inference ? 4 : 16) : 0;
    dec->col_bytes = (size_t)mbs * dec->col_blocks * sizeof(colblk_t);
    if (dec->col_bytes) {
        dec->col_block = dec->config.alloc(dec->config.ctx, dec->col_bytes * dec->pool_size);
        if (!dec->col_block) {
            dec->error = "not enough memory for the reference frames";
            return false;
        }
        for (uint8_t i = 0; i < dec->pool_size; i++) {
            dec->frames[i].col = dec->col_block + dec->col_bytes * i;
        }
    }

    h264_dec_stream_info_t *info = &dec->info;
    memset(info, 0, sizeof(*info));
    info->coded_width = (uint16_t)w;
    info->coded_height = (uint16_t)h;
    info->crop_left = sps->crop_left;
    info->crop_top = sps->crop_top;
    info->width = (uint16_t)(w - sps->crop_left - sps->crop_right);
    info->height = (uint16_t)(h - sps->crop_top - sps->crop_bottom);
    info->full_range = sps->full_range;
    info->matrix_coefficients = sps->matrix;
    info->profile_idc = sps->profile_idc;
    info->level_idc = sps->level_idc;
    info->max_ref_frames = sps->max_num_ref_frames;

    dec->max_long_term_plus1 = 0;
    dec->prev_ref_frame_num = 0;
    dec->need_keyframe = true;
    dec->have_active = true;
    return true;
}

frame_t *h264_free_frame(h264_dec_t *dec) {
    for (uint8_t i = 0; i < dec->pool_size; i++) {
        frame_t *f = &dec->frames[i];
        if (f->ref == REF_NONE && !f->needed_for_output && atomic_load(&f->holds) == 0) return f;
    }
    return NULL;
}

int h264_gap_frames_left(const h264_dec_t *dec) {
    int n = 0;
    for (uint8_t i = 0; i < dec->pool_size; i++) {
        const frame_t *f = &dec->frames[i];
        if (f->ref == REF_NONE && !f->needed_for_output && atomic_load(&f->holds) == 0) n++;
    }
    return n;
}

void h264_discard_waiting(h264_dec_t *dec) {
    for (uint8_t i = 0; i < dec->pool_size; i++) dec->frames[i].needed_for_output = false;
}

static frame_t *next_output(h264_dec_t *dec, int *waiting, int *free_slots) {
    frame_t *best = NULL;
    *waiting = 0;
    *free_slots = 0;
    for (uint8_t i = 0; i < dec->pool_size; i++) {
        frame_t *f = &dec->frames[i];
        if (f->ref == REF_NONE && !f->needed_for_output && atomic_load(&f->holds) == 0) {
            (*free_slots)++;
        }
        if (!f->needed_for_output) continue;
        (*waiting)++;
        if (!best || f->epoch < best->epoch || (f->epoch == best->epoch && f->poc < best->poc)) {
            best = f;
        }
    }
    return best;
}

static void finish_picture(h264_dec_t *dec) {
    const uint32_t total = (uint32_t)dec->mb_w * dec->mb_h;
    if (dec->next_mb < total) dec->pic_error = true;
    while (dec->next_mb < total) h264_conceal_mb(dec, dec->next_mb++);

    frame_t *cur = dec->cur;
    cur->concealed = dec->pic_error;
    cur->tag = dec->cur_tag;
    dec->cur_mmco5 = false;
    if (dec->first_slice.nal_ref_idc) {
        h264_mark_references(dec);
        dec->last_ref = cur;
    }
    if (dec->cur_mmco5) dec->epoch++;
    cur->epoch = dec->epoch;
    cur->needed_for_output = !dec->skip_leading || cur->poc >= dec->leading_poc;
    if (cur->needed_for_output) dec->skip_leading = false;
    dec->cur = NULL;
    dec->in_picture = false;
}

static h264_dec_result_t start_picture(h264_dec_t *dec, const slice_t *s) {
    const pps_t *pps = &dec->pps[s->pps_id];
    if (!activate_sps(dec, pps->sps_id)) return H264_DEC_UNSUPPORTED;
    if (pps->unsupported) {
        dec->error = pps->unsupported;
        return H264_DEC_UNSUPPORTED;
    }
    const bool restarting = dec->need_keyframe;
    if (dec->need_keyframe) {
        if (s->type != SLICE_I) return H264_DEC_NO_PICTURE;
        dec->need_keyframe = false;
    }

    dec->cur = NULL;
    if (s->idr) {
        for (uint8_t i = 0; i < dec->pool_size; i++) dec->frames[i].ref = REF_NONE;
        if (s->no_output_of_prior_pics) h264_discard_waiting(dec);
        dec->epoch++;
        dec->prev_ref_frame_num = 0;
        dec->last_ref = NULL;
    } else {
        const uint32_t max_frame_num = 1u << dec->active.log2_max_frame_num;
        if (s->frame_num != dec->prev_ref_frame_num &&
            s->frame_num != (dec->prev_ref_frame_num + 1) % max_frame_num) {
            h264_handle_frame_num_gap(dec, s->frame_num);
        }
    }

    frame_t *f = h264_free_frame(dec);
    if (!f) {
        dec->error = "no free frame buffer (too many pictures held)";
        return H264_DEC_NO_FRAME;
    }
    dec->cur_poc = h264_compute_poc(dec, s, true);
    if (restarting && !s->idr) {
        dec->skip_leading = true;
        dec->leading_poc = dec->cur_poc;
    }
    f->frame_num = s->frame_num;
    f->frame_num_wrap = s->frame_num;
    f->poc = dec->cur_poc;
    f->long_term_idx = 0;
    f->ref = REF_NONE;
    f->non_existing = false;
    f->concealed = false;
    f->needed_for_output = false;
    dec->cur = f;
    dec->cur_pps = pps;
    dec->cabac_on = pps->cabac;
    h264_build_level_scales(dec);
    dec->first_slice = *s;
    dec->in_picture = true;
    dec->pic_error = false;
    dec->next_mb = 0;
    dec->slice_num = 0;
    dec->ref_count[0] = 0;
    dec->ref_count[1] = 0;
    dec->win_frame = NULL;
    dec->win_disabled = false;
    dec->win_lo = dec->win_hi = 0;
    dec->win_target = 0;
    atomic_store(&dec->win_goal, 0);
    atomic_store(&dec->win_claimed, 0);
    atomic_store(&dec->win_busy, -1);
    atomic_store(&dec->win_floor, 0);
    h264_rows_begin_picture(dec);
    return H264_DEC_OK;
}

static bool same_picture(const slice_t *a, const slice_t *b) {
    return a->frame_num == b->frame_num && a->pps_id == b->pps_id && a->idr == b->idr &&
           (a->nal_ref_idc != 0) == (b->nal_ref_idc != 0);
}

static h264_dec_result_t decode_slice(h264_dec_t *dec, const nal_t *nal, uint8_t nal_type,
                                      uint8_t nal_ref_idc) {
    bits_t *b = &dec->bits;
    if (!bits_init(b, dec->bits_buf, dec->bits_cap, nal->data + 1, nal->len - 1)) {
        return H264_DEC_BAD_DATA;
    }
    slice_t *s = &dec->slice;
    const int parsed = h264_parse_slice_header(dec, b, nal_type, nal_ref_idc, s);
    if (parsed <= 0) {
        if (dec->in_picture) dec->pic_error = true;
        return parsed < 0 ? H264_DEC_UNSUPPORTED : H264_DEC_BAD_DATA;
    }
    if (s->redundant_pic_cnt) return H264_DEC_OK;

    if (dec->in_picture && (s->first_mb == 0 || !same_picture(s, &dec->first_slice))) {
        finish_picture(dec);
    }
    if (!dec->in_picture) {
        const h264_dec_result_t r = start_picture(dec, s);
        if (r != H264_DEC_OK) return r;
    }
    const uint32_t total = (uint32_t)dec->mb_w * dec->mb_h;
    if (s->first_mb >= total || s->first_mb < dec->next_mb) {
        dec->pic_error = true;
        return H264_DEC_BAD_DATA;
    }
    while (dec->next_mb < s->first_mb) {
        dec->pic_error = true;
        h264_conceal_mb(dec, dec->next_mb++);
    }

    if (s->type != SLICE_I) {
        if (!h264_build_ref_list(dec, s)) {
            dec->pic_error = true;
            return H264_DEC_BAD_DATA;
        }
    }
    const bool ok = h264_decode_slice_data(dec);
    dec->slice_num++;
    if (!ok) {
        dec->pic_error = true;
        return H264_DEC_BAD_DATA;
    }
    return H264_DEC_OK;
}

h264_dec_t *h264_dec_create(const h264_dec_config_t *config) {
    if (!config || !config->alloc || !config->free || !config->work) return NULL;
    if ((uintptr_t)config->work & (H264_DEC_WORK_ALIGNMENT - 1)) return NULL;
    h264_dec_t *dec = config->alloc(config->ctx, sizeof(*dec));
    if (!dec) return NULL;
    memset(dec, 0, sizeof(*dec));
    dec->config = *config;
    dec->work_base = config->work;
    if (!h264_build_vlcs(dec)) {
        config->free(config->ctx, dec);
        return NULL;
    }
    dec->bits_cap = BITS_CHUNK;
    dec->bits_buf = h264_work_alloc(dec, BITS_CHUNK + BITS_PADDING);
    if (!dec->bits_buf || !h264_rows_start(dec)) {
        h264_rows_stop(dec);
        config->free(config->ctx, dec);
        return NULL;
    }
    dec->work_tables_end = dec->work_used;
    dec->need_keyframe = true;
    return dec;
}

void h264_dec_destroy(h264_dec_t *dec) {
    if (!dec) return;
    h264_rows_stop(dec);
    release_frames(dec);
    dec->config.free(dec->config.ctx, dec);
}

static h264_dec_result_t decode_packet(h264_dec_t *dec, const uint8_t *data, size_t len,
                                       uint8_t nal_length_size);

h264_dec_result_t h264_dec_decode(h264_dec_t *dec, const uint8_t *data, size_t len,
                                  uint8_t nal_length_size, int64_t tag) {
    dec->error = NULL;
    dec->cur_tag = tag;
    dec->draining = false;
    PROF_START(dec);
    const h264_dec_result_t r = decode_packet(dec, data, len, nal_length_size);
    PROF_STOP(dec, H264_PROF_TOTAL);
    return r;
}

static bool ready_to_output(const h264_dec_t *dec, const frame_t *f, int waiting, int free_slots) {
    if (dec->draining || waiting > dec->reorder_frames || free_slots <= 1) return true;
    if (!dec->have_out_poc || f->epoch != dec->out_epoch) return false;
    return dec->poc_step > 0 && f->poc - dec->out_poc <= dec->poc_step;
}

bool h264_dec_output(h264_dec_t *dec, h264_dec_picture_t *picture) {
    int waiting = 0;
    int free_slots = 0;
    frame_t *f = next_output(dec, &waiting, &free_slots);
    if (!f) return false;
    if (!ready_to_output(dec, f, waiting, free_slots)) return false;
    if (dec->have_out_poc && f->epoch == dec->out_epoch) {
        const int32_t step = f->poc - dec->out_poc;
        if (step > 0 && (!dec->poc_step || step < dec->poc_step)) dec->poc_step = step;
    }
    dec->out_epoch = f->epoch;
    dec->out_poc = f->poc;
    dec->have_out_poc = true;
    f->needed_for_output = false;
    atomic_fetch_add(&f->holds, 1);
    const uint8_t id = (uint8_t)(f - dec->frames);
    picture->packed = f->data;
    picture->packed_bytes = dec->frame_bytes;
    picture->info = dec->info;
    picture->tag = f->tag;
    picture->id = id;
    picture->reference = f->ref != REF_NONE;
    picture->concealed = f->concealed;
    return true;
}

void h264_dec_drain(h264_dec_t *dec) {
    if (dec->in_picture) finish_picture(dec);
    dec->draining = true;
}

static h264_dec_result_t decode_packet(h264_dec_t *dec, const uint8_t *data, size_t len,
                                       uint8_t nal_length_size) {
    h264_k_prepare();
    h264_dec_result_t result = H264_DEC_NO_PICTURE;
    bool bad = false;
    nal_iter_t it;
    nal_t nal;
    nal_iter_init(&it, data, len, nal_length_size);
    while (nal_next(&it, &nal)) {
        const uint8_t header = nal.data[0];
        const uint8_t type = header & 0x1F;
        const uint8_t ref_idc = (header >> 5) & 3;
        if (type == NAL_SPS || type == NAL_PPS) {
            if (dec->in_picture) {
                finish_picture(dec);
                result = H264_DEC_OK;
            }
            bits_t *b = &dec->bits;
            if (!bits_init(b, dec->bits_buf, dec->bits_cap, nal.data + 1, nal.len - 1)) continue;
            if (type == NAL_SPS) h264_parse_sps(dec, b);
            else h264_parse_pps(dec, b);
            continue;
        }
        if (type != NAL_SLICE && type != NAL_IDR) continue;
        if (result == H264_DEC_OK && !dec->in_picture) break;
        const h264_dec_result_t r = decode_slice(dec, &nal, type, ref_idc);
        if (r == H264_DEC_UNSUPPORTED || r == H264_DEC_NO_MEMORY || r == H264_DEC_NO_FRAME) {
            if (dec->in_picture) finish_picture(dec);
            dec->cur = NULL;
            return r;
        }
        if (r == H264_DEC_NO_PICTURE) return H264_DEC_NO_PICTURE;
        if (r == H264_DEC_BAD_DATA) bad = true;
    }
    if (dec->in_picture) {
        finish_picture(dec);
        result = H264_DEC_OK;
    }
    if (result != H264_DEC_OK) return bad ? H264_DEC_BAD_DATA : H264_DEC_NO_PICTURE;
    return H264_DEC_OK;
}

bool h264_dec_stream_info(const h264_dec_t *dec, h264_dec_stream_info_t *info) {
    if (!dec->have_active) return false;
    *info = dec->info;
    return true;
}

void h264_dec_hold(h264_dec_t *dec, uint8_t id) {
    if (id < dec->pool_size) atomic_fetch_add(&dec->frames[id].holds, 1);
}

void h264_dec_release(h264_dec_t *dec, uint8_t id) {
    if (id >= dec->pool_size) return;
    atomic_uchar *holds = &dec->frames[id].holds;
    unsigned char v = atomic_load(holds);
    while (v && !atomic_compare_exchange_weak(holds, &v, (unsigned char)(v - 1))) {}
}

void h264_dec_flush(h264_dec_t *dec) {
    for (uint8_t i = 0; i < dec->pool_size; i++) {
        dec->frames[i].ref = REF_NONE;
        dec->frames[i].needed_for_output = false;
    }
    dec->draining = false;
    dec->have_out_poc = false;
    dec->epoch++;
    dec->in_picture = false;
    dec->cur = NULL;
    dec->last_ref = NULL;
    dec->need_keyframe = true;
    dec->prev_ref_frame_num = 0;
    dec->max_long_term_plus1 = 0;
    dec->prev_poc_msb = 0;
    dec->prev_poc_lsb = 0;
    dec->prev_frame_num_offset = 0;
    dec->prev_frame_num = 0;
    dec->skip_leading = false;
}

bool h264_dec_take_profile(h264_dec_t *dec, uint64_t out[H264_PROF_COUNT]) {
#ifdef H264_DEC_PROFILE
    memcpy(out, dec->prof, sizeof(dec->prof));
    memset(dec->prof, 0, sizeof(dec->prof));
    return true;
#else
    (void)dec;
    (void)out;
    return false;
#endif
}

const char *h264_dec_error(const h264_dec_t *dec) {
    return dec->error;
}

bool h264_dec_probe(const uint8_t *data, size_t len, uint8_t nal_length_size,
                    h264_dec_stream_info_t *info, const char **error) {
    nal_iter_t it;
    nal_t nal;
    nal_iter_init(&it, data, len, nal_length_size);
    while (nal_next(&it, &nal)) {
        if ((nal.data[0] & 0x1F) != NAL_SPS) continue;
        uint8_t local[2 * BITS_MB_RESERVE + BITS_PADDING];
        bits_t b;
        if (!bits_init(&b, local, 2 * BITS_MB_RESERVE, nal.data + 1, nal.len - 1)) continue;
        sps_t sps;
        uint8_t id = 0;
        if (!h264_parse_sps_standalone(&b, &sps, &id)) continue;
        if (sps.unsupported) {
            if (error) *error = sps.unsupported;
            return false;
        }
        memset(info, 0, sizeof(*info));
        info->coded_width = (uint16_t)(sps.mb_width * 16);
        info->coded_height = (uint16_t)(sps.mb_height * 16);
        info->crop_left = sps.crop_left;
        info->crop_top = sps.crop_top;
        info->width = (uint16_t)(info->coded_width - sps.crop_left - sps.crop_right);
        info->height = (uint16_t)(info->coded_height - sps.crop_top - sps.crop_bottom);
        info->full_range = sps.full_range;
        info->matrix_coefficients = sps.matrix;
        info->profile_idc = sps.profile_idc;
        info->level_idc = sps.level_idc;
        info->max_ref_frames = sps.max_num_ref_frames;
        return true;
    }
    if (error) *error = "no H.264 sequence parameter set found";
    return false;
}

bool h264_dec_droppable(const uint8_t *data, size_t len, uint8_t nal_length_size) {
    nal_iter_t it;
    nal_t nal;
    nal_iter_init(&it, data, len, nal_length_size);
    while (nal_next(&it, &nal)) {
        const uint8_t type = nal.data[0] & 0x1F;
        if (type == NAL_SLICE || type == NAL_IDR) return ((nal.data[0] >> 5) & 3) == 0;
    }
    return false;
}
