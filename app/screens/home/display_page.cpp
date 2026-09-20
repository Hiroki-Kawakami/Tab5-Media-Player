/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "display_page.hpp"
#include <functional>
#include <initializer_list>
#include "grouped_list.hpp"
#include "settings.hpp"
#include "widgets.hpp"

static constexpr int32_t kPadding = 24;
static constexpr int32_t kRowGap = 12;
static constexpr int32_t kKnobSize = 28;
static constexpr uint32_t kSegmentTrackColor = 0xe0e0e0;
static constexpr uint32_t kSegmentActiveColor = 0x2196f3;

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

static void segmented_paint(lv_obj_t *segmented, int active) {
    for (uint32_t i = 0; i < lv_obj_get_child_count(segmented); i++) {
        auto button = lv_obj_get_child(segmented, i);
        const bool on = (int)i == active;
        lv_obj_set_style_bg_color(
            button, lv_color_hex(on ? kSegmentActiveColor : kSegmentTrackColor), 0);
        lv_obj_set_style_text_color(button, on ? lv_color_white() : lv_color_black(), 0);
    }
}

static lv_obj_t *segmented_create(lv_obj_t *row, std::initializer_list<const char *> labels,
                                  int active, std::function<void(lv_obj_t *, int)> on_select) {
    auto segmented = lv_container_create(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(segmented, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(segmented, 4, 0);
    lv_obj_set_style_pad_column(segmented, 4, 0);
    lv_obj_set_style_radius(segmented, 12, 0);
    lv_obj_set_style_bg_color(segmented, lv_color_hex(kSegmentTrackColor), 0);
    lv_obj_set_style_bg_opa(segmented, LV_OPA_COVER, 0);

    int index = 0;
    for (auto label : labels) {
        auto button = lv_button_create(segmented, LV_BUTTON_STYLE_SECONDARY);
        lv_button_set_text(button, label);
        lv_obj_set_style_pad_hor(button, 22, 0);
        lv_obj_set_style_pad_ver(button, 12, 0);
        lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [segmented, index, on_select](lv_event_t *) {
            on_select(segmented, index);
        });
        index++;
    }
    segmented_paint(segmented, active);
    return segmented;
}

static void color_mode_failed(lv_obj_t *obj, esp_err_t err) {
    auto modal = lv_modal_open(lv_obj_get_screen(obj));
    lv_modal_title_create(modal, "Color Mode");
    lv_modal_message_create(
        modal, (std::string("The display could not be switched.\n") + esp_err_to_name(err)).c_str());
    lv_modal_button_create(modal, "Close", LV_MODAL_BUTTON_TYPE_PRIMARY,
                           [modal](lv_event_t *) { lv_modal_close(modal); });
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
    lv_obj_add_event_fn(slider, LV_EVENT_RELEASED, [](lv_event_t *) { settings_commit(); });

    lv_hor_separator_create(section);

    row = setting_row_create(section, "Color Mode");
    const int color_mode = settings_display_pixel_format() == BSP_PIXEL_FORMAT_RGB888 ? 1 : 0;
    segmented_create(row, {"16-bit", "24-bit"}, color_mode,
                     [](lv_obj_t *segmented, int index) {
        const auto format = index == 1 ? BSP_PIXEL_FORMAT_RGB888 : BSP_PIXEL_FORMAT_RGB565;
        if (format == settings_display_pixel_format()) return;
        segmented_paint(segmented, index);
        lv_async_call([segmented, format] {
            const esp_err_t err = settings_set_display_pixel_format(format);
            if (err == ESP_OK) {
                settings_commit();
                return;
            }
            if (!lv_obj_is_valid(segmented)) return;
            segmented_paint(segmented, format == BSP_PIXEL_FORMAT_RGB888 ? 0 : 1);
            color_mode_failed(segmented, err);
        });
    });
}
