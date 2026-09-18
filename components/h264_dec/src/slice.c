/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_internal.h"

static const char *kUnsupportedSlice = "unsupported H.264 stream (SP/SI slices)";

int h264_parse_slice_header(struct h264_dec *dec, bits_t *b, uint8_t nal_type, uint8_t nal_ref_idc,
                            slice_t *s) {
    s->nal_ref_idc = nal_ref_idc;
    s->idr = nal_type == NAL_IDR;
    s->reorder_count[0] = 0;
    s->reorder_count[1] = 0;
    s->mmco_count = 0;
    s->adaptive_marking = false;
    s->long_term_reference = false;
    s->redundant_pic_cnt = 0;
    s->no_output_of_prior_pics = false;
    s->disable_deblock = 0;
    s->alpha_offset = 0;
    s->beta_offset = 0;

    s->first_mb = bits_ue(b);
    const uint32_t slice_type = bits_ue(b);
    if (slice_type > 9) return 0;
    const uint32_t type = slice_type % 5;
    if (type != SLICE_P && type != SLICE_I && type != SLICE_B) {
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
    s->poc_lsb = 0;
    s->delta_poc_bottom = 0;
    s->delta_poc[0] = 0;
    s->delta_poc[1] = 0;
    if (sps->poc_type == 0) {
        s->poc_lsb = bits_u(b, sps->log2_max_poc_lsb);
        if (pps->bottom_field_pic_order_present) s->delta_poc_bottom = bits_se(b);
    } else if (sps->poc_type == 1 && !sps->delta_pic_order_always_zero) {
        s->delta_poc[0] = bits_se(b);
        if (pps->bottom_field_pic_order_present) s->delta_poc[1] = bits_se(b);
    }
    if (pps->redundant_pic_cnt_present) s->redundant_pic_cnt = bits_ue(b);

    s->num_ref_idx_active = 0;
    s->num_ref_idx_l1 = 0;
    s->direct_spatial = false;
    if (type == SLICE_B) s->direct_spatial = bits_u1(b);
    if (type == SLICE_P || type == SLICE_B) {
        uint32_t l0 = pps->num_ref_idx_default;
        uint32_t l1 = pps->num_ref_idx_l1_default;
        if (bits_u1(b)) {
            l0 = bits_ue(b) + 1;
            if (type == SLICE_B) l1 = bits_ue(b) + 1;
        }
        if (l0 > MAX_REFS || l1 > MAX_REFS) return 0;
        s->num_ref_idx_active = (uint8_t)l0;
        s->num_ref_idx_l1 = type == SLICE_B ? (uint8_t)l1 : 0;
        const int lists = type == SLICE_B ? 2 : 1;
        for (int list = 0; list < lists; list++) {
            if (!bits_u1(b)) continue;
            for (;;) {
                const uint32_t idc = bits_ue(b);
                if (idc == 3) break;
                uint8_t *count = &s->reorder_count[list];
                if (idc > 2 || *count >= MAX_REORDER || bits_overrun(b)) return 0;
                s->reorder_idc[list][*count] = (uint8_t)idc;
                s->reorder_value[list][*count] = bits_ue(b);
                (*count)++;
            }
        }
    }

    s->weighted = false;
    s->luma_denom = 0;
    s->chroma_denom = 0;
    if ((pps->weighted_pred && type == SLICE_P) ||
        (pps->weighted_bipred_idc == 1 && type == SLICE_B)) {
        const uint32_t luma_denom = bits_ue(b);
        const uint32_t chroma_denom = bits_ue(b);
        if (luma_denom > 7 || chroma_denom > 7) return 0;
        s->weighted = true;
        s->luma_denom = (uint8_t)luma_denom;
        s->chroma_denom = (uint8_t)chroma_denom;
        const int lists = type == SLICE_B ? 2 : 1;
        for (int list = 0; list < lists; list++) {
            const int count = list ? s->num_ref_idx_l1 : s->num_ref_idx_active;
            for (int i = 0; i < count; i++) {
                weight_t *w = s->weights[list][i];
                w[0].weight = (int16_t)(1 << luma_denom);
                w[0].offset = 0;
                if (bits_u1(b)) {
                    w[0].weight = (int16_t)bits_se(b);
                    w[0].offset = (int16_t)bits_se(b);
                }
                w[1].weight = w[2].weight = (int16_t)(1 << chroma_denom);
                w[1].offset = w[2].offset = 0;
                if (bits_u1(b)) {
                    for (int comp = 1; comp < 3; comp++) {
                        w[comp].weight = (int16_t)bits_se(b);
                        w[comp].offset = (int16_t)bits_se(b);
                    }
                }
                if (bits_overrun(b)) return 0;
            }
        }
    }

    if (nal_ref_idc) {
        if (s->idr) {
            s->no_output_of_prior_pics = bits_u1(b);
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

    s->cabac_init_idc = 0;
    if (pps->cabac && type != SLICE_I) {
        const uint32_t idc = bits_ue(b);
        if (idc > 2) return 0;
        s->cabac_init_idc = (uint8_t)idc;
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

int32_t h264_compute_poc(struct h264_dec *dec, const slice_t *s, bool commit) {
    const sps_t *sps = &dec->active;
    int32_t poc = 0;
    int32_t top = 0;
    if (sps->poc_type == 0) {
        const int32_t max_lsb = 1 << sps->log2_max_poc_lsb;
        int32_t prev_msb = s->idr ? 0 : dec->prev_poc_msb;
        const int32_t prev_lsb = s->idr ? 0 : dec->prev_poc_lsb;
        const int32_t lsb = (int32_t)s->poc_lsb;
        if (lsb < prev_lsb && prev_lsb - lsb >= max_lsb / 2) prev_msb += max_lsb;
        else if (lsb > prev_lsb && lsb - prev_lsb > max_lsb / 2) prev_msb -= max_lsb;
        top = prev_msb + lsb;
        poc = imin(top, top + s->delta_poc_bottom);
        if (commit && s->nal_ref_idc) {
            dec->prev_poc_msb = prev_msb;
            dec->prev_poc_lsb = lsb;
        }
    } else {
        const int32_t max_frame_num = 1 << sps->log2_max_frame_num;
        int32_t offset;
        if (s->idr) offset = 0;
        else if (dec->prev_frame_num > s->frame_num) offset = dec->prev_frame_num_offset + max_frame_num;
        else offset = dec->prev_frame_num_offset;
        if (sps->poc_type == 1) {
            int32_t abs_frame = sps->poc_cycle_length ? offset + s->frame_num : 0;
            if (!s->nal_ref_idc && abs_frame > 0) abs_frame--;
            int32_t expected = 0;
            if (abs_frame > 0) {
                const int32_t cycles = (abs_frame - 1) / sps->poc_cycle_length;
                const int32_t in_cycle = (abs_frame - 1) % sps->poc_cycle_length;
                expected = cycles * sps->poc_cycle_sum;
                for (int32_t i = 0; i <= in_cycle; i++) expected += sps->offset_for_ref_frame[i];
            }
            if (!s->nal_ref_idc) expected += sps->offset_for_non_ref_pic;
            top = expected + s->delta_poc[0];
            poc = imin(top, top + sps->offset_for_top_to_bottom + s->delta_poc[1]);
        } else {
            const int32_t doubled = 2 * (offset + s->frame_num);
            top = s->idr ? 0 : (s->nal_ref_idc ? doubled : doubled - 1);
            poc = top;
        }
        if (commit) dec->prev_frame_num_offset = offset;
    }
    if (commit) {
        dec->prev_frame_num = s->frame_num;
        dec->cur_poc_top = top;
    }
    return poc;
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

static void sort_by_wrap(frame_t **list, uint8_t n) {
    for (uint8_t i = 1; i < n; i++) {
        frame_t *f = list[i];
        int j = i - 1;
        while (j >= 0 && list[j]->frame_num_wrap < f->frame_num_wrap) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = f;
    }
}

static void sort_by_poc(frame_t **list, uint8_t n, bool ascending) {
    for (uint8_t i = 1; i < n; i++) {
        frame_t *f = list[i];
        int j = i - 1;
        while (j >= 0 && (ascending ? list[j]->poc > f->poc : list[j]->poc < f->poc)) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = f;
    }
}

static void sort_by_long_idx(frame_t **list, uint8_t n) {
    for (uint8_t i = 1; i < n; i++) {
        frame_t *f = list[i];
        int j = i - 1;
        while (j >= 0 && list[j]->long_term_idx > f->long_term_idx) {
            list[j + 1] = list[j];
            j--;
        }
        list[j + 1] = f;
    }
}

static bool apply_modification(struct h264_dec *dec, const slice_t *s, int list_idx,
                               uint8_t active) {
    frame_t **list = dec->ref_list[list_idx];
    const int32_t max_pic = 1 << dec->active.log2_max_frame_num;
    const int32_t curr = s->frame_num;
    int32_t pred = curr;
    uint8_t ref_idx = 0;
    bool ok = true;
    for (uint8_t k = 0; k < s->reorder_count[list_idx] && ref_idx < active; k++) {
        frame_t *pic;
        if (s->reorder_idc[list_idx][k] < 2) {
            const int32_t abs_diff = (int32_t)s->reorder_value[list_idx][k] + 1;
            if (abs_diff > max_pic) return false;
            int32_t no_wrap;
            if (s->reorder_idc[list_idx][k] == 0) {
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
            pic = find_long(dec, (int32_t)s->reorder_value[list_idx][k]);
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
    if (!ok) dec->pic_error = true;
    return true;
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
    sort_by_long_idx(longs, n_long);

    const uint8_t active0 = s->num_ref_idx_active;
    frame_t **list0 = dec->ref_list[0];
    uint8_t count = 0;
    if (s->type == SLICE_B) {
        frame_t *before[MAX_POOL];
        frame_t *after[MAX_POOL];
        uint8_t n_before = 0, n_after = 0;
        for (uint8_t i = 0; i < n_short; i++) {
            if (shorts[i]->poc < dec->cur_poc) before[n_before++] = shorts[i];
            else after[n_after++] = shorts[i];
        }
        sort_by_poc(before, n_before, false);
        sort_by_poc(after, n_after, true);
        for (uint8_t i = 0; i < n_before && count <= MAX_REFS; i++) list0[count++] = before[i];
        for (uint8_t i = 0; i < n_after && count <= MAX_REFS; i++) list0[count++] = after[i];
        for (uint8_t i = 0; i < n_long && count <= MAX_REFS; i++) list0[count++] = longs[i];
        for (uint8_t i = count; i <= MAX_REFS; i++) list0[i] = NULL;

        frame_t **list1 = dec->ref_list[1];
        uint8_t count1 = 0;
        for (uint8_t i = 0; i < n_after && count1 <= MAX_REFS; i++) list1[count1++] = after[i];
        for (uint8_t i = 0; i < n_before && count1 <= MAX_REFS; i++) list1[count1++] = before[i];
        for (uint8_t i = 0; i < n_long && count1 <= MAX_REFS; i++) list1[count1++] = longs[i];
        for (uint8_t i = count1; i <= MAX_REFS; i++) list1[i] = NULL;
        if (count1 > 1 && count1 == count) {
            bool same = true;
            for (uint8_t i = 0; i < count; i++) same &= list0[i] == list1[i];
            if (same) {
                frame_t *tmp = list1[0];
                list1[0] = list1[1];
                list1[1] = tmp;
            }
        }
        dec->ref_count[0] = active0;
        dec->ref_count[1] = s->num_ref_idx_l1;
        if (!apply_modification(dec, s, 0, active0)) return false;
        if (!apply_modification(dec, s, 1, s->num_ref_idx_l1)) return false;
        h264_build_implicit_weights(dec);
        return true;
    }

    sort_by_wrap(shorts, n_short);
    for (uint8_t i = 0; i < n_short && count <= MAX_REFS; i++) list0[count++] = shorts[i];
    for (uint8_t i = 0; i < n_long && count <= MAX_REFS; i++) list0[count++] = longs[i];
    for (uint8_t i = count < active0 ? count : active0; i <= MAX_REFS; i++) list0[i] = NULL;
    dec->ref_count[0] = active0;
    dec->ref_count[1] = 0;
    return apply_modification(dec, s, 0, active0);
}

void h264_build_implicit_weights(struct h264_dec *dec) {
    if (dec->cur_pps->weighted_bipred_idc != 2) return;
    const int32_t cur = dec->cur_poc;
    for (uint8_t i = 0; i < dec->ref_count[0] && i < MAX_REFS; i++) {
        const frame_t *f0 = dec->ref_list[0][i];
        for (uint8_t j = 0; j < dec->ref_count[1] && j < MAX_REFS; j++) {
            const frame_t *f1 = dec->ref_list[1][j];
            int w1 = 32;
            if (f0 && f1 && f0->ref != REF_LONG && f1->ref != REF_LONG) {
                const int td = clip3(-128, 127, f1->poc - f0->poc);
                if (td) {
                    const int tb = clip3(-128, 127, cur - f0->poc);
                    const int tx = (16384 + iabs(td / 2)) / td;
                    const int dsf = clip3(-1024, 1023, (tb * tx + 32) >> 6) >> 2;
                    if (dsf >= -64 && dsf <= 128) w1 = dsf;
                }
            }
            dec->implicit_w[i][j] = (int16_t)w1;
        }
    }
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
    dec->cur_mmco5 = mmco5;
    if (mmco5) {
        cur->frame_num = 0;
        cur->frame_num_wrap = 0;
        dec->prev_poc_lsb = dec->cur_poc_top - dec->cur_poc;
        cur->poc = 0;
        dec->cur_poc = 0;
        dec->prev_ref_frame_num = 0;
        dec->prev_frame_num = 0;
        dec->prev_frame_num_offset = 0;
        dec->prev_poc_msb = 0;
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
        if (h264_gap_frames_left(dec) < 2) break;
        frame_t *f = h264_free_frame(dec);
        if (!f) break;
        update_wraps(dec, (uint16_t)unused);
        sliding_window(dec);
        f->frame_num = (uint16_t)unused;
        f->frame_num_wrap = (int32_t)unused;
        f->non_existing = true;
        f->concealed = true;
        f->needed_for_output = false;
        f->ref = REF_SHORT;
        if (dec->active.poc_type != 0) {
            slice_t gap = { 0 };
            gap.frame_num = (uint16_t)unused;
            gap.nal_ref_idc = 1;
            f->poc = h264_compute_poc(dec, &gap, true);
        } else {
            f->poc = dec->cur_poc;
        }
        dec->prev_ref_frame_num = (uint16_t)unused;
        unused = (unused + 1) % max;
    }
}
