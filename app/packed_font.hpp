/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "lvgl.hpp"
#include "resources.h"
#include <cstddef>
#include <cstdint>

// Renders a resgen font pack (see docs/fonts.md) as an lv_font_t, so it can be
// hung off a built-in font's `fallback`. Decoded glyphs are kept as A8 masks in
// PSRAM and handed to LVGL directly through the static-bitmap path.
class PackedFont {
public:
    static constexpr std::size_t kDefaultCacheBytes = 128 * 1024;

    explicit PackedFont(const resgen_font_pack_t &pack, std::size_t cache_bytes = kDefaultCacheBytes);
    ~PackedFont();
    PackedFont(const PackedFont &) = delete;
    PackedFont &operator=(const PackedFont &) = delete;

    const lv_font_t *font() const { return &font_; }
    int32_t max_ascent() const { return pack_.max_ascent; }
    int32_t max_descent() const { return pack_.max_descent; }

private:
    struct Slot {
        uint8_t *bitmap;
        uint32_t glyph;
        uint32_t bytes;
    };

    static constexpr uint32_t kSlotCount = 512;

    static bool get_glyph_dsc_cb(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                                 uint32_t letter, uint32_t next);
    static const void *get_glyph_bitmap_cb(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *draw_buf);

    int32_t find(uint16_t codepoint) const;
    const uint8_t *cached(uint32_t index, const resgen_glyph_t &glyph);
    void decode(const resgen_glyph_t &glyph, uint8_t *dst, uint32_t stride) const;
    void release(Slot &slot);

    const resgen_font_pack_t &pack_;
    lv_font_t font_{};
    uint8_t opa_[16]{};
    Slot *slots_ = nullptr;
    std::size_t cache_bytes_;
    std::size_t used_bytes_ = 0;
    uint32_t hand_ = 0;
};
