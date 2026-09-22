/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "packed_font.hpp"
#include "esp_heap_caps.h"

namespace {

constexpr uint32_t kCompressedFlag = 0x80000000u;
constexpr uint32_t kOffsetMask = 0x7FFFFFFFu;

// Mirrors the decoder in lv_font_fmt_txt.c, which the resgen encoder targets:
// a value costs bpp bits, a value equal to the previous one switches to repeat
// mode where every further repeat is one bit, and the 11th repeat is followed
// by a 6 bit count that runs out on a value rather than on a repeat.
class Reader {
public:
    Reader(const uint8_t *data, uint8_t bpp, bool compressed)
        : data_(data), bpp_(bpp), compressed_(compressed) {}

    uint8_t next() {
        if (!compressed_) return uint8_t(bits(bpp_));

        switch (state_) {
        case State::Single: {
            const uint8_t value = uint8_t(bits(bpp_));
            if (started_ && value == previous_) {
                state_ = State::Repeat;
                repeats_ = 0;
            }
            started_ = true;
            previous_ = value;
            return value;
        }
        case State::Repeat:
            repeats_++;
            if (bits(1)) {
                if (repeats_ != 11) return previous_;
                repeats_ = bits(6);
                if (repeats_) {
                    state_ = State::Count;
                    return previous_;
                }
            }
            return single();
        default:
            if (--repeats_) return previous_;
            return single();
        }
    }

private:
    enum class State : uint8_t { Single, Repeat, Count };

    uint8_t single() {
        previous_ = uint8_t(bits(bpp_));
        state_ = State::Single;
        return previous_;
    }

    uint32_t bits(uint8_t count) {
        const uint32_t byte = position_ >> 3;
        const uint32_t offset = position_ & 7;
        const uint32_t window = (uint32_t(data_[byte]) << 16) |
                                (uint32_t(data_[byte + 1]) << 8) | data_[byte + 2];
        position_ += count;
        return (window >> (24 - offset - count)) & ((1u << count) - 1);
    }

    const uint8_t *data_;
    uint32_t position_ = 0;
    uint8_t bpp_;
    bool compressed_;
    bool started_ = false;
    uint8_t previous_ = 0;
    uint8_t repeats_ = 0;
    State state_ = State::Single;
};

}  // namespace

PackedFont::PackedFont(const resgen_font_pack_t &pack, std::size_t cache_bytes)
    : pack_(pack), cache_bytes_(cache_bytes) {
    const uint8_t levels = uint8_t((1u << pack.bpp) - 1);
    for (uint32_t i = 0; i <= levels; i++) opa_[i] = uint8_t((i * 255 + levels / 2) / levels);

    slots_ = static_cast<Slot *>(heap_caps_calloc(kSlotCount, sizeof(Slot), MALLOC_CAP_SPIRAM));

    font_.get_glyph_dsc = get_glyph_dsc_cb;
    font_.get_glyph_bitmap = get_glyph_bitmap_cb;
    font_.line_height = pack.line_height;
    font_.base_line = pack.base_line;
    font_.subpx = LV_FONT_SUBPX_NONE;
    font_.kerning = LV_FONT_KERNING_NONE;
    font_.static_bitmap = slots_ != nullptr;
    font_.underline_position = pack.underline_position;
    font_.underline_thickness = pack.underline_thickness;
    font_.dsc = this;
}

PackedFont::~PackedFont() {
    if (!slots_) return;
    for (uint32_t i = 0; i < kSlotCount; i++) release(slots_[i]);
    heap_caps_free(slots_);
}

int32_t PackedFont::find(uint16_t codepoint) const {
    uint32_t low = 0;
    uint32_t high = pack_.glyph_count;
    while (low < high) {
        const uint32_t middle = (low + high) / 2;
        if (pack_.codepoints[middle] < codepoint) low = middle + 1;
        else high = middle;
    }
    if (low >= pack_.glyph_count || pack_.codepoints[low] != codepoint) return -1;
    return int32_t(low);
}

bool PackedFont::get_glyph_dsc_cb(const lv_font_t *font, lv_font_glyph_dsc_t *out,
                                  uint32_t letter, uint32_t) {
    auto *self = static_cast<const PackedFont *>(font->dsc);
    if (letter > 0xFFFF) return false;
    const int32_t index = self->find(uint16_t(letter));
    if (index < 0) return false;

    const resgen_glyph_t &glyph = self->pack_.glyphs[index];
    out->adv_w = uint16_t((glyph.adv_w + 8) >> 4);
    out->box_w = glyph.box_w;
    out->box_h = glyph.box_h;
    out->ofs_x = glyph.ofs_x;
    out->ofs_y = glyph.ofs_y;
    out->stride = glyph.box_w;
    out->format = LV_FONT_GLYPH_FORMAT_A8;
    out->is_placeholder = false;
    out->gid.index = uint32_t(index);
    return true;
}

const void *PackedFont::get_glyph_bitmap_cb(lv_font_glyph_dsc_t *dsc, lv_draw_buf_t *draw_buf) {
    auto *self = const_cast<PackedFont *>(static_cast<const PackedFont *>(dsc->resolved_font->dsc));
    const resgen_glyph_t &glyph = self->pack_.glyphs[dsc->gid.index];
    if (!glyph.box_w || !glyph.box_h) return nullptr;

    if (draw_buf) {
        self->decode(glyph, draw_buf->data, draw_buf->header.stride);
        return draw_buf;
    }
    return self->cached(dsc->gid.index, glyph);
}

void PackedFont::release(Slot &slot) {
    if (!slot.bitmap) return;
    heap_caps_free(slot.bitmap);
    used_bytes_ -= slot.bytes;
    slot = {};
}

const uint8_t *PackedFont::cached(uint32_t index, const resgen_glyph_t &glyph) {
    Slot &slot = slots_[index % kSlotCount];
    if (slot.bitmap && slot.glyph == index) return slot.bitmap;
    release(slot);

    const uint32_t bytes = uint32_t(glyph.box_w) * glyph.box_h;
    while (used_bytes_ && used_bytes_ + bytes > cache_bytes_) {
        Slot &victim = slots_[hand_];
        hand_ = (hand_ + 1) % kSlotCount;
        if (&victim != &slot) release(victim);
    }

    auto *bitmap = static_cast<uint8_t *>(heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM));
    if (!bitmap) return nullptr;
    decode(glyph, bitmap, glyph.box_w);
    slot = {bitmap, index, bytes};
    used_bytes_ += bytes;
    return bitmap;
}

void PackedFont::decode(const resgen_glyph_t &glyph, uint8_t *dst, uint32_t stride) const {
    const bool compressed = (glyph.bitmap & kCompressedFlag) != 0;
    Reader reader(pack_.data + (glyph.bitmap & kOffsetMask), pack_.bpp, compressed);
    const bool prefilter = compressed && pack_.prefilter;

    uint8_t previous[256] = {};
    for (uint32_t y = 0; y < glyph.box_h; y++) {
        uint8_t *row = dst + std::size_t(y) * stride;
        for (uint32_t x = 0; x < glyph.box_w; x++) {
            uint8_t level = reader.next();
            if (prefilter) {
                level ^= previous[x];
                previous[x] = level;
            }
            row[x] = opa_[level];
        }
    }
}
