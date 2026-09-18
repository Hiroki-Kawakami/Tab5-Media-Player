/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "mpeg2_internal.h"

static bool build(struct mpeg2_dec *dec, vlc_t *vlc, const vlc_code_t *codes, uint16_t count,
                  uint8_t root) {
    uint8_t *sub_bits = dec->config.alloc(dec->config.ctx, 1u << root);
    if (!sub_bits) return false;
    const uint32_t root_size = 1u << root;
    memset(sub_bits, 0, root_size);
    uint32_t total = root_size;
    for (uint16_t i = 0; i < count; i++) {
        if (codes[i].len <= root) continue;
        const uint32_t prefix = codes[i].code >> (codes[i].len - root);
        const uint8_t bits = (uint8_t)(codes[i].len - root);
        if (bits > sub_bits[prefix]) sub_bits[prefix] = bits;
    }
    for (uint32_t p = 0; p < root_size; p++) {
        if (sub_bits[p]) total += 1u << sub_bits[p];
    }
    uint32_t *table = mpeg2_work_alloc(dec, total * sizeof(uint32_t));
    if (!table) {
        dec->config.free(dec->config.ctx, sub_bits);
        return false;
    }
    memset(table, 0, total * sizeof(uint32_t));

    uint32_t next = root_size;
    for (uint32_t p = 0; p < root_size; p++) {
        if (!sub_bits[p]) continue;
        table[p] = (next << 8) | ((uint32_t)sub_bits[p] << 1) | VLC_SUB;
        next += 1u << sub_bits[p];
    }
    for (uint16_t i = 0; i < count; i++) {
        const vlc_code_t *c = &codes[i];
        const uint32_t value = (uint32_t)((int32_t)c->value * 256);
        if (c->len <= root) {
            const uint32_t shift = root - c->len;
            const uint32_t first = (uint32_t)c->code << shift;
            for (uint32_t k = 0; k < (1u << shift); k++) {
                table[first + k] = value | ((uint32_t)c->len << 1);
            }
            continue;
        }
        const uint32_t prefix = c->code >> (c->len - root);
        const uint32_t base = table[prefix] >> 8;
        const uint8_t bits = sub_bits[prefix];
        const uint8_t len = (uint8_t)(c->len - root);
        const uint32_t suffix = c->code & ((1u << len) - 1);
        const uint32_t shift = bits - len;
        for (uint32_t k = 0; k < (1u << shift); k++) {
            table[base + (suffix << shift) + k] = value | ((uint32_t)len << 1);
        }
    }
    dec->config.free(dec->config.ctx, sub_bits);
    vlc->table = table;
    vlc->root = root;
    return true;
}

bool mpeg2_vlc_build(struct mpeg2_dec *dec, vlc_t *vlc, const vlc_code_t *codes, uint16_t count,
                     uint8_t root, bool signed_levels) {
    if (!signed_levels) return build(dec, vlc, codes, count, root);
    vlc_code_t *expanded = dec->config.alloc(dec->config.ctx, sizeof(vlc_code_t) * count * 2u);
    if (!expanded) return false;
    uint16_t n = 0;
    for (uint16_t i = 0; i < count; i++) {
        const vlc_code_t *c = &codes[i];
        if (c->value == DCT_EOB || c->value == DCT_ESCAPE) {
            expanded[n++] = *c;
            continue;
        }
        const int run = c->value & 31;
        const int level = c->value >> 5;
        expanded[n++] = (vlc_code_t){ c->code << 1, (uint8_t)(c->len + 1), (int16_t)(run + level * 32) };
        expanded[n++] = (vlc_code_t){ (c->code << 1) | 1, (uint8_t)(c->len + 1), (int16_t)(run - level * 32) };
    }
    const bool ok = build(dec, vlc, expanded, n, root);
    dec->config.free(dec->config.ctx, expanded);
    return ok;
}

bool mpeg2_build_vlcs(struct mpeg2_dec *dec) {
    return mpeg2_vlc_build(dec, &dec->vlc_mbai, mpeg2_mbai_codes, mpeg2_mbai_count, 6, false) &&
           mpeg2_vlc_build(dec, &dec->vlc_mbtype[0], mpeg2_mbtype_i_codes, mpeg2_mbtype_i_count, 2, false) &&
           mpeg2_vlc_build(dec, &dec->vlc_mbtype[1], mpeg2_mbtype_p_codes, mpeg2_mbtype_p_count, 6, false) &&
           mpeg2_vlc_build(dec, &dec->vlc_mbtype[2], mpeg2_mbtype_b_codes, mpeg2_mbtype_b_count, 6, false) &&
           mpeg2_vlc_build(dec, &dec->vlc_cbp, mpeg2_cbp_codes, mpeg2_cbp_count, 9, false) &&
           mpeg2_vlc_build(dec, &dec->vlc_mv, mpeg2_mv_codes, mpeg2_mv_count, 7, false) &&
           mpeg2_vlc_build(dec, &dec->vlc_dc[0], mpeg2_dc_luma_codes, mpeg2_dc_luma_count, 9, false) &&
           mpeg2_vlc_build(dec, &dec->vlc_dc[1], mpeg2_dc_chroma_codes, mpeg2_dc_chroma_count, 10, false) &&
           mpeg2_vlc_build(dec, &dec->vlc_dct[0], mpeg2_dct0_codes, mpeg2_dct0_count, 10, true) &&
           mpeg2_vlc_build(dec, &dec->vlc_dct[1], mpeg2_dct1_codes, mpeg2_dct1_count, 10, true);
}
