/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "display_page.hpp"
#include "grouped_list.hpp"
#include "settings.hpp"
#include "widgets.hpp"

static constexpr int32_t kPadding = 24;
static constexpr int32_t kRowGap = 12;
static constexpr int32_t kKnobSize = 28;

static lv_obj_t *setting_row_create(lv_obj_t *section, const char *label) {
    auto row = lv_container_create(section, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    auto text = lv_label_create(row);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_style_text_font(text, lv_widgets_body_font(), 0);
    lv_label_set_text(text, label);
    return row;
}

static lv_obj_t *value_label_create(lv_obj_t *row) {
    auto value = lv_label_create(row);
    lv_obj_set_style_text_font(value, lv_widgets_body_font(), 0);
    lv_obj_set_style_text_color(value, lv_color_hex(0x808080), 0);
    return value;
}

static lv_obj_t *slider_create(lv_obj_t *section, int32_t min, int32_t max, int32_t value) {
    auto box = lv_container_create(section, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(box, kKnobSize / 2, 0);

    auto slider = lv_slider_create(box);
    lv_obj_set_flex_grow(slider, 1);
    lv_slider_set_range(slider, min, max);
    lv_slider_set_value(slider, value, LV_ANIM_OFF);
    lv_obj_set_style_width(slider, kKnobSize, LV_PART_KNOB);
    lv_obj_set_style_height(slider, kKnobSize, LV_PART_KNOB);
    lv_obj_set_style_radius(slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    return slider;
}

void DisplayPage::build(lv_obj_t *contents) {
    lv_obj_set_style_bg_color(contents, lv_color_hex(0xeeeeee), 0);
    lv_obj_set_style_bg_opa(contents, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(contents, kPadding, 0);
    lv_obj_set_style_pad_row(contents, kRowGap, 0);

    auto section = lv_grouped_section_create(contents);
    lv_obj_set_style_pad_all(section, kPadding, 0);
    lv_obj_set_style_pad_row(section, kRowGap, 0);

    auto row = setting_row_create(section, "Brightness");
    auto value = value_label_create(row);
    lv_label_set_text_fmt(value, "%d%%", settings_display_brightness());

    auto slider = slider_create(section, kMinDisplayBrightness, 100,
                                settings_display_brightness());
    lv_obj_add_event_fn(slider, LV_EVENT_VALUE_CHANGED, [slider, value](lv_event_t *) {
        const int brightness = lv_slider_get_value(slider);
        settings_set_display_brightness(brightness);
        lv_label_set_text_fmt(value, "%d%%", brightness);
    });
}
