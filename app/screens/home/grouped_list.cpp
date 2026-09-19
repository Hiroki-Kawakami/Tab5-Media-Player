/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "grouped_list.hpp"
#include "widgets.hpp"

namespace {

constexpr int32_t kRowHeight = 88;
constexpr int32_t kPadding = 24;
constexpr uint32_t kSelectedColor = 0x2196f3;

bool style_initialized = false;
lv_style_t section_style, row_style, row_pressed_style, row_checked_style;

void style_init() {
    if (style_initialized) return;

    lv_style_init(&section_style);
    lv_style_set_bg_color(&section_style, lv_color_white());
    lv_style_set_bg_opa(&section_style, LV_OPA_COVER);
    lv_style_set_radius(&section_style, 16);
    lv_style_set_clip_corner(&section_style, true);
    lv_style_set_layout(&section_style, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&section_style, LV_FLEX_FLOW_COLUMN);

    lv_style_init(&row_style);
    lv_style_set_width(&row_style, LV_PCT(100));
    lv_style_set_height(&row_style, kRowHeight);
    lv_style_set_pad_hor(&row_style, kPadding);
    lv_style_set_pad_column(&row_style, kPadding);
    lv_style_set_layout(&row_style, LV_LAYOUT_FLEX);
    lv_style_set_flex_flow(&row_style, LV_FLEX_FLOW_ROW);
    lv_style_set_flex_main_place(&row_style, LV_FLEX_ALIGN_START);
    lv_style_set_flex_cross_place(&row_style, LV_FLEX_ALIGN_CENTER);
    lv_style_set_flex_track_place(&row_style, LV_FLEX_ALIGN_CENTER);
    lv_style_set_text_font(&row_style, lv_widgets_body_font());

    lv_style_init(&row_pressed_style);
    lv_style_set_bg_color(&row_pressed_style, lv_color_black());
    lv_style_set_bg_opa(&row_pressed_style, 35);

    lv_style_init(&row_checked_style);
    lv_style_set_bg_color(&row_checked_style, lv_color_hex(kSelectedColor));
    lv_style_set_bg_opa(&row_checked_style, LV_OPA_COVER);
    lv_style_set_text_color(&row_checked_style, lv_color_white());

    style_initialized = true;
}

}

lv_obj_t *lv_grouped_section_create(lv_obj_t *parent, const char *title) {
    style_init();
    if (title) {
        auto label = lv_label_create(parent);
        lv_label_set_text(label, title);
        lv_obj_set_style_text_font(label, lv_widgets_body_font(), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x808080), 0);
        lv_obj_set_style_pad_hor(label, kPadding, 0);
    }
    auto section = lv_container_create(parent);
    lv_obj_add_style(section, &section_style, 0);
    lv_obj_set_size(section, LV_PCT(100), LV_SIZE_CONTENT);
    return section;
}

lv_obj_t *lv_grouped_row_create(lv_obj_t *section, const char *icon, const char *label) {
    style_init();
    if (lv_obj_get_child_count(section) > 0) {
        auto inset = lv_container_create(section);
        lv_obj_set_size(inset, LV_PCT(100), 1);
        lv_obj_set_style_pad_hor(inset, kPadding, 0);
        lv_hor_separator_create(inset);
    }

    auto row = lv_button_create(section);
    lv_obj_remove_style_all(row);
    lv_obj_add_style(row, &row_style, 0);
    lv_obj_add_style(row, &row_pressed_style, LV_STATE_PRESSED);
    lv_obj_add_style(row, &row_checked_style, LV_STATE_CHECKED);

    auto icon_label = lv_label_create(row);
    lv_obj_set_width(icon_label, 48);
    lv_obj_set_style_text_align(icon_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(icon_label, icon);

    auto text = lv_label_create(row);
    lv_obj_set_flex_grow(text, 1);
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(text, label);

    auto arrow = lv_label_create(row);
    lv_obj_set_width(arrow, 48);
    lv_obj_set_style_text_align(arrow, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x808080), 0);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    return row;
}

void lv_grouped_row_set_arrow_visible(lv_obj_t *row, bool visible) {
    lv_obj_set_flag(lv_obj_get_child(row, 2), LV_OBJ_FLAG_HIDDEN, !visible);
}
