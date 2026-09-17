/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_internal.h"

static const char *kUnsupportedProfile =
    "unsupported H.264 stream (re-encode with -profile:v baseline)";

static bool is_high_profile(uint8_t profile) {
    switch (profile) {
    case 100: case 110: case 122: case 244: case 44: case 83: case 86:
    case 118: case 128: case 138: case 139: case 134: case 135:
        return true;
    default:
        return false;
    }
}

static void skip_scaling_list(bits_t *b, int size) {
    int last = 8;
    int next = 8;
    for (int i = 0; i < size && next && !bits_overrun(b); i++) {
        next = (last + bits_se(b) + 256) % 256;
        last = next ? next : last;
    }
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
        for (int i = 0; i < 6; i++) bits_ue(b);
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
            sps.unsupported = kUnsupportedProfile;
            const int lists = chroma_format != 3 ? 8 : 12;
            for (int i = 0; i < lists; i++) {
                if (bits_u1(b)) skip_scaling_list(b, i < 6 ? 16 : 64);
            }
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
        bits_se(b);
        bits_se(b);
        const uint32_t cycle = bits_ue(b);
        if (cycle > 255) return false;
        for (uint32_t i = 0; i < cycle && !bits_overrun(b); i++) bits_se(b);
    }
    const uint32_t refs = bits_ue(b);
    if (refs > MAX_REFS) return false;
    sps.max_num_ref_frames = (uint8_t)refs;
    sps.gaps_allowed = bits_u1(b);
    const uint32_t mb_w = bits_ue(b) + 1;
    const uint32_t map_h = bits_ue(b) + 1;
    const bool frame_mbs_only = bits_u1(b);
    if (!frame_mbs_only) bits_skip(b, 1);
    bits_skip(b, 1);
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
    if (bits_u1(b)) pps.unsupported = kUnsupportedProfile;
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
    pps.weighted_pred = bits_u1(b);
    bits_skip(b, 2);
    if (pps.weighted_pred && !pps.unsupported) pps.unsupported = kUnsupportedProfile;
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
    if (bits_more_data(b)) {
        const bool transform8x8 = bits_u1(b);
        const bool scaling = bits_u1(b);
        if (transform8x8 || scaling) {
            if (!pps.unsupported) pps.unsupported = kUnsupportedProfile;
        }
        if (scaling) {
            const int lists = 6 + (transform8x8 ? 2 : 0);
            for (int i = 0; i < lists; i++) {
                if (bits_u1(b)) skip_scaling_list(b, i < 6 ? 16 : 64);
            }
        }
        const int32_t second = bits_se(b);
        if (second < -12 || second > 12) return false;
        pps.chroma_qp_offset[1] = (int8_t)second;
    }
    if (bits_overrun(b)) return false;
    pps.valid = true;
    dec->pps[pps_id] = pps;
    return true;
}
