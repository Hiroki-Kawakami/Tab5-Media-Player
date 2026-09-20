/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "display_page.hpp"
#include "settings.hpp"
#include "settings_widgets.hpp"
#include "widgets.hpp"

static void color_mode_failed(lv_obj_t *obj, esp_err_t err) {
    auto modal = lv_modal_open(lv_obj_get_screen(obj));
    lv_modal_title_create(modal, "Color Mode");
    lv_modal_message_create(
        modal, (std::string("The display could not be switched.\n") + esp_err_to_name(err)).c_str());
    lv_modal_button_create(modal, "Close", LV_MODAL_BUTTON_TYPE_PRIMARY,
                           [modal](lv_event_t *) { lv_modal_close(modal); });
}

void DisplayPage::build(lv_obj_t *contents) {
    lv_setting_page_style(contents);
    auto section = lv_setting_section_create(contents);

    auto row = lv_setting_row_create(section, "Brightness");
    auto value = lv_setting_value_create(row);
    lv_label_set_text_fmt(value, "%d%%", settings_display_brightness());

    auto slider = lv_setting_slider_create(section, kMinDisplayBrightness, 100,
                                           settings_display_brightness());
    lv_obj_add_event_fn(slider, LV_EVENT_VALUE_CHANGED, [slider, value](lv_event_t *) {
        const int brightness = lv_slider_get_value(slider);
        settings_set_display_brightness(brightness);
        lv_label_set_text_fmt(value, "%d%%", brightness);
    });
    lv_obj_add_event_fn(slider, LV_EVENT_RELEASED, [](lv_event_t *) { settings_commit(); });

    lv_hor_separator_create(section);

    row = lv_setting_row_create(section, "Color Mode");
    const int color_mode = settings_display_pixel_format() == BSP_PIXEL_FORMAT_RGB888 ? 1 : 0;
    lv_setting_segmented_create(row, {"16-bit", "24-bit"}, color_mode,
                                [](lv_obj_t *segmented, int index) {
        const auto format = index == 1 ? BSP_PIXEL_FORMAT_RGB888 : BSP_PIXEL_FORMAT_RGB565;
        if (format == settings_display_pixel_format()) return;
        lv_setting_segmented_set_active(segmented, index);
        lv_async_call([segmented, format] {
            const esp_err_t err = settings_set_display_pixel_format(format);
            if (err == ESP_OK) {
                settings_commit();
                return;
            }
            if (!lv_obj_is_valid(segmented)) return;
            lv_setting_segmented_set_active(segmented,
                                            format == BSP_PIXEL_FORMAT_RGB888 ? 0 : 1);
            color_mode_failed(segmented, err);
        });
    });

    lv_hor_separator_create(section);

    row = lv_setting_row_create(section, "Rotation Lock");
    lv_setting_switch_create(row, settings_rotation_locked(), [](lv_obj_t *, bool locked) {
        settings_set_rotation_lock(locked);
        settings_commit();
    });
}
