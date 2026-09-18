/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_internal.h"

static const char *kUnsupportedProfile =
    "unsupported H.264 profile (Baseline, Main and High only)";

static bool is_high_profile(uint8_t profile) {
    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86:
    case 118: case 128: case 138: case 139: case 134: case 135:
        return true;
    default:
        return false;
    }
}

static void read_scaling_list(bits_t *b, uint8_t *out, int size, const uint8_t *def) {
    const uint8_t *scan = size == 16 ? h264_zigzag4x4 : h264_zigzag8x8;
    int last = 8;
    int next = 8;
    uint8_t values[64];
    for (int i = 0; i < size; i++) {
        if (next && !bits_overrun(b)) {
            next = (last + bits_se(b) + 256) % 256;
            if (i == 0 && !next) {
                memcpy(out, def, (size_t)size);
                return;
            }
            last = next ? next : last;
        }
        values[i] = (uint8_t)last;
    }
    for (int i = 0; i < size; i++) out[scan[i]] = values[i];
}

static void scaling_lists(bits_t *b, uint8_t scaling4[6][16], uint8_t scaling8[2][64], int count,
                          bool pps_fallback) {
    for (int i = 0; i < count; i++) {
        uint8_t *dst = i < 6 ? scaling4[i] : scaling8[i - 6];
        const int size = i < 6 ? 16 : 64;
        const uint8_t *def = i < 6 ? h264_default_scaling4[i < 3 ? 0 : 1]
                                   : h264_default_scaling8[(i - 6) & 1];
        if (bits_u1(b)) {
            read_scaling_list(b, dst, size, def);
            continue;
        }
        if (i == 0 || i == 3 || i >= 6) {
            if (!pps_fallback) memcpy(dst, def, (size_t)size);
        } else {
            memcpy(dst, scaling4[i - 1], 16);
        }
    }
}

static void flat_scaling(uint8_t scaling4[6][16], uint8_t scaling8[2][64]) {
    memset(scaling4, 16, 6 * 16);
    memset(scaling8, 16, 2 * 64);
}

static void skip_hrd(bits_t *b) {
    const uint32_t count = bits_ue(b) + 1;
    bits_skip(b, 8);
    for (uint32_t i = 0; i < count && i < 32 && !bits_overrun(b); i++) {
        bits_ue(b);
        bits_ue(b);
        bits_skip(b, 1);
    }
    bits_skip(b, 20);
}

static void parse_vui(bits_t *b, sps_t *sps) {
    if (bits_u1(b)) {
        if (bits_u(b, 8) == 255) bits_skip(b, 32);
    }
    if (bits_u1(b)) bits_skip(b, 1);
    if (bits_u1(b)) {
        bits_skip(b, 3);
        sps->full_range = bits_u1(b);
        if (bits_u1(b)) {
            bits_skip(b, 16);
            sps->matrix = (uint8_t)bits_u(b, 8);
        }
    }
    if (bits_u1(b)) {
        bits_ue(b);
        bits_ue(b);
    }
    if (bits_u1(b)) bits_skip(b, 65);
    const bool nal_hrd = bits_u1(b);
    if (nal_hrd) skip_hrd(b);
    const bool vcl_hrd = bits_u1(b);
    if (vcl_hrd) skip_hrd(b);
    if (nal_hrd || vcl_hrd) bits_skip(b, 1);
    bits_skip(b, 1);
    if (bits_u1(b)) {
        bits_skip(b, 1);
        bits_ue(b);
        bits_ue(b);
        bits_ue(b);
        bits_ue(b);
        const uint32_t reorder = bits_ue(b);
        const uint32_t buffering = bits_ue(b);
        if (reorder <= MAX_REFS && buffering <= MAX_REFS && reorder <= buffering) {
            sps->has_bitstream_restriction = true;
            sps->num_reorder_frames = (uint8_t)reorder;
            sps->max_dec_frame_buffering = (uint8_t)buffering;
        }
    }
}

bool h264_parse_sps_standalone(bits_t *b, sps_t *out, uint8_t *id) {
    sps_t sps;
    memset(&sps, 0, sizeof(sps));
    sps.matrix = 2;
    sps.profile_idc = (uint8_t)bits_u(b, 8);
    sps.constraint_flags = (uint8_t)bits_u(b, 8);
    sps.level_idc = (uint8_t)bits_u(b, 8);
    const uint32_t sps_id = bits_ue(b);
    if (sps_id >= MAX_SPS) return false;

    flat_scaling(sps.scaling4, sps.scaling8);
    if (is_high_profile(sps.profile_idc)) {
        const uint32_t chroma_format = bits_ue(b);
        if (chroma_format == 3) bits_skip(b, 1);
        const uint32_t luma_depth = bits_ue(b);
        const uint32_t chroma_depth = bits_ue(b);
        bits_skip(b, 1);
        const bool scaling = bits_u1(b);
        if (chroma_format != 1) sps.unsupported = "unsupported H.264 chroma format (4:2:0 only)";
        if (luma_depth || chroma_depth) sps.unsupported = "unsupported H.264 bit depth (8-bit only)";
        if (scaling) {
            sps.has_scaling = true;
            scaling_lists(b, sps.scaling4, sps.scaling8, chroma_format != 3 ? 8 : 12, false);
        }
    }

    const uint32_t log2_max_frame_num = bits_ue(b) + 4;
    if (log2_max_frame_num > 16) return false;
    sps.log2_max_frame_num = (uint8_t)log2_max_frame_num;
    const uint32_t poc_type = bits_ue(b);
    if (poc_type > 2) return false;
    sps.poc_type = (uint8_t)poc_type;
    if (poc_type == 0) {
        const uint32_t log2_max_poc = bits_ue(b) + 4;
        if (log2_max_poc > 16) return false;
        sps.log2_max_poc_lsb = (uint8_t)log2_max_poc;
    } else if (poc_type == 1) {
        sps.delta_pic_order_always_zero = bits_u1(b);
        sps.offset_for_non_ref_pic = bits_se(b);
        sps.offset_for_top_to_bottom = bits_se(b);
        const uint32_t cycle = bits_ue(b);
        if (cycle >= MAX_POC_CYCLE) return false;
        sps.poc_cycle_length = (uint16_t)cycle;
        for (uint32_t i = 0; i < cycle && !bits_overrun(b); i++) {
            sps.offset_for_ref_frame[i] = bits_se(b);
            sps.poc_cycle_sum += sps.offset_for_ref_frame[i];
        }
    }
    const uint32_t refs = bits_ue(b);
    if (refs > MAX_REFS) return false;
    sps.max_num_ref_frames = (uint8_t)refs;
    sps.gaps_allowed = bits_u1(b);
    const uint32_t mb_w = bits_ue(b) + 1;
    const uint32_t map_h = bits_ue(b) + 1;
    const bool frame_mbs_only = bits_u1(b);
    if (!frame_mbs_only) bits_skip(b, 1);
    sps.direct_8x8_inference = bits_u1(b);
    if (mb_w > 1024 || map_h > 1024) return false;
    sps.mb_width = (uint16_t)mb_w;
    sps.mb_height = (uint16_t)(frame_mbs_only ? map_h : map_h * 2);
    if (!frame_mbs_only) sps.unsupported = "interlaced H.264 is not supported";

    if (bits_u1(b)) {
        const uint32_t l = bits_ue(b), r = bits_ue(b), t = bits_ue(b), bo = bits_ue(b);
        const uint32_t unit_y = frame_mbs_only ? 2 : 4;
        if ((l + r) * 2 >= mb_w * 16 || (t + bo) * unit_y >= sps.mb_height * 16u) return false;
        sps.crop_left = (uint16_t)(l * 2);
        sps.crop_right = (uint16_t)(r * 2);
        sps.crop_top = (uint16_t)(t * unit_y);
        sps.crop_bottom = (uint16_t)(bo * unit_y);
    }
    if (bits_u1(b)) parse_vui(b, &sps);
    if (bits_overrun(b)) return false;

    if (!sps.unsupported && sps.profile_idc != 66 && !(sps.constraint_flags & 0x40) &&
        sps.profile_idc != 77 && sps.profile_idc != 100) {
        sps.unsupported = kUnsupportedProfile;
    }
    sps.valid = true;
    *out = sps;
    *id = (uint8_t)sps_id;
    return true;
}

bool h264_parse_sps(struct h264_dec *dec, bits_t *b) {
    sps_t sps;
    uint8_t id;
    if (!h264_parse_sps_standalone(b, &sps, &id)) return false;
    dec->sps[id] = sps;
    return true;
}

bool h264_parse_pps(struct h264_dec *dec, bits_t *b) {
    pps_t pps;
    memset(&pps, 0, sizeof(pps));
    const uint32_t pps_id = bits_ue(b);
    const uint32_t sps_id = bits_ue(b);
    if (pps_id >= MAX_PPS || sps_id >= MAX_SPS) return false;
    pps.sps_id = (uint8_t)sps_id;
    pps.cabac = bits_u1(b);
    pps.bottom_field_pic_order_present = bits_u1(b);
    const uint32_t groups = bits_ue(b) + 1;
    if (groups > 1) {
        pps.unsupported = "H.264 slice groups (FMO) are not supported";
        pps.valid = true;
        dec->pps[pps_id] = pps;
        return true;
    }
    const uint32_t ref_l0 = bits_ue(b) + 1;
    const uint32_t ref_l1 = bits_ue(b) + 1;
    if (ref_l0 > 32 || ref_l1 > 32) return false;
    pps.num_ref_idx_default = (uint8_t)ref_l0;
    pps.num_ref_idx_l1_default = (uint8_t)ref_l1;
    pps.weighted_pred = bits_u1(b);
    pps.weighted_bipred_idc = (uint8_t)bits_u(b, 2);
    const int32_t qp = 26 + bits_se(b);
    bits_se(b);
    const int32_t cqp = bits_se(b);
    if (qp < 0 || qp > 51 || cqp < -12 || cqp > 12) return false;
    pps.pic_init_qp = (int8_t)qp;
    pps.chroma_qp_offset[0] = (int8_t)cqp;
    pps.chroma_qp_offset[1] = (int8_t)cqp;
    pps.deblocking_control = bits_u1(b);
    pps.constrained_intra_pred = bits_u1(b);
    pps.redundant_pic_cnt_present = bits_u1(b);
    const sps_t *sps = &dec->sps[sps_id];
    if (sps->valid && sps->has_scaling) {
        memcpy(pps.scaling4, sps->scaling4, sizeof(pps.scaling4));
        memcpy(pps.scaling8, sps->scaling8, sizeof(pps.scaling8));
        pps.has_scaling = true;
    } else {
        flat_scaling(pps.scaling4, pps.scaling8);
    }
    if (bits_more_data(b)) {
        pps.transform_8x8_mode = bits_u1(b);
        const bool scaling = bits_u1(b);
        if (scaling) {
            pps.has_scaling = true;
            if (!sps->valid || !sps->has_scaling) {
                for (int i = 0; i < 6; i++) {
                    memcpy(pps.scaling4[i], h264_default_scaling4[i < 3 ? 0 : 1], 16);
                }
                for (int i = 0; i < 2; i++) memcpy(pps.scaling8[i], h264_default_scaling8[i], 64);
            }
            scaling_lists(b, pps.scaling4, pps.scaling8,
                          6 + (pps.transform_8x8_mode ? 2 : 0), true);
        }
        const int32_t second = bits_se(b);
        if (second < -12 || second > 12) return false;
        pps.chroma_qp_offset[1] = (int8_t)second;
    }
    if (bits_overrun(b)) return false;
    pps.valid = true;
    dec->pps[pps_id] = pps;
    dec->ls_pps = NULL;
    return true;
}

void h264_build_level_scales(struct h264_dec *dec) {
    const pps_t *pps = dec->cur_pps;
    if (dec->ls_pps == pps) return;
    dec->ls_pps = pps;
    for (int list = 0; list < 6; list++) {
        for (int m = 0; m < 6; m++) {
            int16_t *out = dec->ls4 + (list * 6 + m) * 16;
            for (int i = 0; i < 16; i++) {
                out[i] = (int16_t)(pps->scaling4[list][i] * h264_dequant4[m][i]);
            }
        }
    }
    for (int list = 0; list < 2; list++) {
        for (int m = 0; m < 6; m++) {
            int16_t *out = dec->ls8 + (list * 6 + m) * 64;
            for (int i = 0; i < 64; i++) {
                out[i] = (int16_t)(pps->scaling8[list][i] * h264_dequant8[m][i]);
            }
        }
    }
}
