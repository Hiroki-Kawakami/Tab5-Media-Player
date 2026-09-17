/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_internal.h"

static const char *kUnsupportedSlice =
    "unsupported H.264 stream (B/SP/SI slices; re-encode with -profile:v baseline)";

int h264_parse_slice_header(struct h264_dec *dec, bits_t *b, uint8_t nal_type, uint8_t nal_ref_idc,
                            slice_t *s) {
    s->nal_ref_idc = nal_ref_idc;
    s->idr = nal_type == NAL_IDR;
    s->reorder_count = 0;
    s->mmco_count = 0;
    s->adaptive_marking = false;
    s->long_term_reference = false;
    s->redundant_pic_cnt = 0;
    s->disable_deblock = 0;
    s->alpha_offset = 0;
    s->beta_offset = 0;

    s->first_mb = bits_ue(b);
    const uint32_t slice_type = bits_ue(b);
    if (slice_type > 9) return 0;
    const uint32_t type = slice_type % 5;
    if (type != SLICE_P && type != SLICE_I) {
        dec->error = kUnsupportedSlice;
        return -1;
    }
    s->type = (uint8_t)type;
    if (s->idr && type != SLICE_I) return 0;

    const uint32_t pps_id = bits_ue(b);
    if (pps_id >= MAX_PPS || !dec->pps[pps_id].valid) {
        dec->error = "slice refers to a missing PPS";
        return 0;
    }
    const pps_t *pps = &dec->pps[pps_id];
    const sps_t *sps = &dec->sps[pps->sps_id];
    if (!sps->valid) {
        dec->error = "slice refers to a missing SPS";
        return 0;
    }
    if (sps->unsupported || pps->unsupported) {
        dec->error = sps->unsupported ? sps->unsupported : pps->unsupported;
        return -1;
    }
    s->pps_id = (uint8_t)pps_id;
    s->frame_num = (uint16_t)bits_u(b, sps->log2_max_frame_num);
    if (s->idr) bits_ue(b);
    if (sps->poc_type == 0) {
        bits_skip(b, sps->log2_max_poc_lsb);
        if (pps->bottom_field_pic_order_present) bits_se(b);
    } else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
        bits_se(b);
        if (pps->bottom_field_pic_order_present) bits_se(b);
    }
    if (pps->redundant_pic_cnt_present) s->redundant_pic_cnt = bits_ue(b);

    s->num_ref_idx_active = 0;
    if (type == SLICE_P) {
        uint32_t refs = pps->num_ref_idx_default;
        if (bits_u1(b)) refs = bits_ue(b) + 1;
        if (refs > MAX_REFS) return 0;
        s->num_ref_idx_active = (uint8_t)refs;
        if (bits_u1(b)) {
            for (;;) {
                const uint32_t idc = bits_ue(b);
                if (idc == 3) break;
                if (idc > 2 || s->reorder_count >= MAX_REORDER || bits_overrun(b)) return 0;
                s->reorder_idc[s->reorder_count] = (uint8_t)idc;
                s->reorder_value[s->reorder_count] = bits_ue(b);
                s->reorder_count++;
            }
        }
    }

    if (nal_ref_idc) {
        if (s->idr) {
            bits_skip(b, 1);
            s->long_term_reference = bits_u1(b);
        } else {
            s->adaptive_marking = bits_u1(b);
            if (s->adaptive_marking) {
                for (;;) {
                    const uint32_t op = bits_ue(b);
                    if (op == 0) break;
                    if (op > 6 || s->mmco_count >= MAX_MMCO || bits_overrun(b)) return 0;
                    mmco_t *m = &s->mmco[s->mmco_count++];
                    m->op = (uint8_t)op;
                    m->a = 0;
                    m->b = 0;
                    if (op == 1 || op == 3) m->a = bits_ue(b);
                    if (op == 2) m->a = bits_ue(b);
                    if (op == 3) m->b = bits_ue(b);
                    if (op == 6 || op == 4) m->a = bits_ue(b);
                }
            }
        }
    }

    const int32_t qp = pps->pic_init_qp + bits_se(b);
    if (qp < 0 || qp > 51) return 0;
    s->qp = (int8_t)qp;
    if (pps->deblocking_control) {
        const uint32_t idc = bits_ue(b);
        if (idc > 2) return 0;
        s->disable_deblock = (uint8_t)idc;
        if (idc != 1) {
            const int32_t alpha = bits_se(b);
            const int32_t beta = bits_se(b);
            if (alpha < -6 || alpha > 6 || beta < -6 || beta > 6) return 0;
            s->alpha_offset = (int8_t)(alpha * 2);
            s->beta_offset = (int8_t)(beta * 2);
        }
    }
    if (bits_overrun(b)) return 0;
    return 1;
}

static void update_wraps(struct h264_dec *dec, uint16_t frame_num) {
    const int32_t max = 1 << dec->active.log2_max_frame_num;
    for (uint8_t i = 0; i < dec->pool_size; i++) {
        frame_t *f = &dec->frames[i];
        if (f->ref != REF_SHORT) continue;
        f->frame_num_wrap = f->frame_num > frame_num ? (int32_t)f->frame_num - max : f->frame_num;
    }
}

static frame_t *find_short(struct h264_dec *dec, int32_t pic_num) {
    for (uint8_t i = 0; i < dec->pool_size; i++) {
        frame_t *f = &dec->frames[i];
        if (f->ref == REF_SHORT && f->frame_num_wrap == pic_num && f != dec->cur) return f;
    }
    return NULL;
}

static frame_t *find_long(struct h264_dec *dec, int32_t idx) {
    for (uint8_t i = 0; i < dec->pool_size; i++) {
        frame_t *f = &dec->frames[i];
        if (f->ref == REF_LONG && f->long_term_idx == idx && f != dec->cur) return f;
    }
    return NULL;
}

bool h264_build_ref_list(struct h264_dec *dec, const slice_t *s) {
    update_wraps(dec, s->frame_num);
    frame_t *shorts[MAX_POOL];
    frame_t *longs[MAX_POOL];
    uint8_t n_short = 0, n_long = 0;
    for (uint8_t i = 0; i < dec->pool_size; i++) {
        frame_t *f = &dec->frames[i];
        if (f == dec->cur) continue;
        if (f->ref == REF_SHORT) shorts[n_short++] = f;
        else if (f->ref == REF_LONG) longs[n_long++] = f;
    }
    for (uint8_t i = 1; i < n_short; i++) {
        frame_t *f = shorts[i];
        int j = i - 1;
        while (j >= 0 && shorts[j]->frame_num_wrap < f->frame_num_wrap) {
            shorts[j + 1] = shorts[j];
            j--;
        }
        shorts[j + 1] = f;
    }
    for (uint8_t i = 1; i < n_long; i++) {
        frame_t *f = longs[i];
        int j = i - 1;
        while (j >= 0 && longs[j]->long_term_idx > f->long_term_idx) {
            longs[j + 1] = longs[j];
            j--;
        }
        longs[j + 1] = f;
    }

    const uint8_t active = s->num_ref_idx_active;
    frame_t **list = dec->ref_list;
    uint8_t count = 0;
    for (uint8_t i = 0; i < n_short && count <= MAX_REFS; i++) list[count++] = shorts[i];
    for (uint8_t i = 0; i < n_long && count <= MAX_REFS; i++) list[count++] = longs[i];
    for (uint8_t i = count < active ? count : active; i <= MAX_REFS; i++) list[i] = NULL;

    const int32_t max_pic = 1 << dec->active.log2_max_frame_num;
    const int32_t curr = s->frame_num;
    int32_t pred = curr;
    uint8_t ref_idx = 0;
    bool ok = true;
    for (uint8_t k = 0; k < s->reorder_count && ref_idx < active; k++) {
        frame_t *pic;
        if (s->reorder_idc[k] < 2) {
            const int32_t abs_diff = (int32_t)s->reorder_value[k] + 1;
            if (abs_diff > max_pic) return false;
            int32_t no_wrap;
            if (s->reorder_idc[k] == 0) {
                no_wrap = pred - abs_diff;
                if (no_wrap < 0) no_wrap += max_pic;
            } else {
                no_wrap = pred + abs_diff;
                if (no_wrap >= max_pic) no_wrap -= max_pic;
            }
            pred = no_wrap;
            const int32_t pic_num = no_wrap > curr ? no_wrap - max_pic : no_wrap;
            pic = find_short(dec, pic_num);
        } else {
            pic = find_long(dec, (int32_t)s->reorder_value[k]);
        }
        if (!pic) ok = false;
        for (uint8_t c = active; c > ref_idx; c--) list[c] = list[c - 1];
        list[ref_idx++] = pic;
        uint8_t n = ref_idx;
        for (uint8_t c = ref_idx; c <= active; c++) {
            if (list[c] != pic) list[n++] = list[c];
        }
    }
    for (uint8_t i = active; i <= MAX_REFS; i++) list[i] = NULL;
    dec->ref_count = active;
    if (!ok) dec->pic_error = true;
    return true;
}

static void sliding_window(struct h264_dec *dec) {
    const uint8_t max_refs = dec->active.max_num_ref_frames ? dec->active.max_num_ref_frames : 1;
    for (;;) {
        uint8_t total = 0;
        frame_t *oldest = NULL;
        for (uint8_t i = 0; i < dec->pool_size; i++) {
            frame_t *f = &dec->frames[i];
            if (f == dec->cur || f->ref == REF_NONE) continue;
            total++;
            if (f->ref == REF_SHORT && (!oldest || f->frame_num_wrap < oldest->frame_num_wrap)) oldest = f;
        }
        if (total < max_refs || !oldest) return;
        oldest->ref = REF_NONE;
    }
}

void h264_mark_references(struct h264_dec *dec) {
    const slice_t *s = &dec->first_slice;
    frame_t *cur = dec->cur;
    if (s->idr) {
        if (s->long_term_reference) {
            cur->ref = REF_LONG;
            cur->long_term_idx = 0;
            dec->max_long_term_plus1 = 1;
        } else {
            cur->ref = REF_SHORT;
            dec->max_long_term_plus1 = 0;
        }
        dec->prev_ref_frame_num = s->frame_num;
        return;
    }

    update_wraps(dec, s->frame_num);
    bool mmco5 = false;
    bool cur_long = false;
    if (s->adaptive_marking) {
        const int32_t curr = s->frame_num;
        for (uint8_t k = 0; k < s->mmco_count; k++) {
            const mmco_t *m = &s->mmco[k];
            frame_t *f;
            switch (m->op) {
            case 1:
                f = find_short(dec, curr - (int32_t)(m->a + 1));
                if (f) f->ref = REF_NONE;
                break;
            case 2:
                f = find_long(dec, (int32_t)m->a);
                if (f) f->ref = REF_NONE;
                break;
            case 3:
                f = find_short(dec, curr - (int32_t)(m->a + 1));
                if (f) {
                    frame_t *old = find_long(dec, (int32_t)m->b);
                    if (old && old != f) old->ref = REF_NONE;
                    f->ref = REF_LONG;
                    f->long_term_idx = (uint8_t)m->b;
                }
                break;
            case 4:
                dec->max_long_term_plus1 = (uint8_t)m->a;
                for (uint8_t i = 0; i < dec->pool_size; i++) {
                    frame_t *g = &dec->frames[i];
                    if (g->ref == REF_LONG && g->long_term_idx >= m->a) g->ref = REF_NONE;
                }
                break;
            case 5:
                for (uint8_t i = 0; i < dec->pool_size; i++) {
                    if (&dec->frames[i] != cur) dec->frames[i].ref = REF_NONE;
                }
                dec->max_long_term_plus1 = 0;
                mmco5 = true;
                break;
            case 6:
                f = find_long(dec, (int32_t)m->a);
                if (f) f->ref = REF_NONE;
                cur->ref = REF_LONG;
                cur->long_term_idx = (uint8_t)m->a;
                cur_long = true;
                break;
            default:
                break;
            }
        }
    } else {
        sliding_window(dec);
    }
    if (!cur_long) {
        if (s->adaptive_marking) sliding_window(dec);
        cur->ref = REF_SHORT;
    }
    if (mmco5) {
        cur->frame_num = 0;
        cur->frame_num_wrap = 0;
        dec->prev_ref_frame_num = 0;
    } else {
        dec->prev_ref_frame_num = s->frame_num;
    }
}

void h264_handle_frame_num_gap(struct h264_dec *dec, uint16_t frame_num) {
    const uint32_t max = 1u << dec->active.log2_max_frame_num;
    uint32_t unused = (dec->prev_ref_frame_num + 1) % max;
    const uint32_t gap = (frame_num + max - unused) % max;
    const uint32_t refs = dec->active.max_num_ref_frames ? dec->active.max_num_ref_frames : 1;
    if (gap > refs) unused = (frame_num + max - refs) % max;
    while (unused != frame_num) {
        frame_t *f = h264_free_frame(dec);
        if (!f) break;
        update_wraps(dec, (uint16_t)unused);
        sliding_window(dec);
        f->frame_num = (uint16_t)unused;
        f->frame_num_wrap = (int32_t)unused;
        f->non_existing = true;
        f->concealed = true;
        f->ref = REF_SHORT;
        dec->prev_ref_frame_num = (uint16_t)unused;
        unused = (unused + 1) % max;
    }
}
