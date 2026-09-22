/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "ui_font.hpp"
#include "packed_font.hpp"
#include "widgets/fonts.hpp"
#include "esp_heap_caps.h"
#include <algorithm>
#include <new>

namespace {

struct UiFonts {
    PackedFont body_jp{noto_sans_jp_24};
    PackedFont title_jp{noto_sans_jp_38};
    lv_font_t body{};
    lv_font_t title{};
};

UiFonts *s_fonts = nullptr;

// copy: the built-in fonts are const. NotoSansJP reaches further above and
// below the baseline than Montserrat at the same size, so the chain keeps
// Montserrat's metrics and only grows them where a kanji would be clipped.
lv_font_t chain(const lv_font_t &latin, const PackedFont &japanese) {
    lv_font_t font = latin;
    font.fallback = japanese.font();
    const int32_t base_line = std::max(latin.base_line, japanese.max_descent());
    const int32_t ascent = std::max(latin.line_height - latin.base_line, japanese.max_ascent());
    font.base_line = base_line;
    font.line_height = ascent + base_line;
    return font;
}

}  // namespace

void ui_font_init() {
    if (s_fonts) return;
    void *memory = heap_caps_malloc(sizeof(UiFonts), MALLOC_CAP_SPIRAM);
    if (!memory) return;

    s_fonts = new (memory) UiFonts();
    s_fonts->body = chain(lv_font_montserrat_24, s_fonts->body_jp);
    s_fonts->title = chain(lv_font_montserrat_38, s_fonts->title_jp);
}

const lv_font_t *lv_widgets_font(lv_widgets_font_role_t role) {
    if (!s_fonts) return nullptr;
    switch (role) {
    case LV_WIDGETS_FONT_BODY:
        return &s_fonts->body;
    case LV_WIDGETS_FONT_TITLE:
        return &s_fonts->title;
    default:
        return nullptr;
    }
}
