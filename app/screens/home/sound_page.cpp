/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "sound_page.hpp"
#include "settings.hpp"
#include "settings_widgets.hpp"
#include "widgets.hpp"

static void set_volume_text(lv_obj_t *value, int volume) {
    lv_label_set_text_fmt(value, "%s  %d",
                          settings_volume_is_headphone() ? "Headphone" : "Speaker", volume);
}

void SoundPage::build(lv_obj_t *contents) {
    lv_setting_page_style(contents);
    auto section = lv_setting_section_create(contents);

    auto row = lv_setting_row_create(section, "Volume");
    auto value = lv_setting_value_create(row);
    set_volume_text(value, settings_volume());

    auto slider = lv_setting_slider_create(section, 0, 100, settings_volume());
    lv_obj_add_event_fn(slider, LV_EVENT_VALUE_CHANGED, [slider, value](lv_event_t *) {
        const int volume = lv_slider_get_value(slider);
        settings_set_volume(volume);
        set_volume_text(value, volume);
    });
    lv_obj_add_event_fn(slider, LV_EVENT_RELEASED, [](lv_event_t *) { settings_commit(); });
    settings_volume_observe(slider, [slider, value](int volume) {
        if (lv_obj_has_state(slider, LV_STATE_PRESSED)) return;
        lv_slider_set_value(slider, volume, LV_ANIM_OFF);
        set_volume_text(value, volume);
    });

    lv_hor_separator_create(section);

    row = lv_setting_row_create(section, "Equalizer");
    lv_setting_switch_create(row, settings_equalizer_enabled(), [](lv_obj_t *, bool enabled) {
        settings_set_equalizer_enabled(enabled);
        settings_commit();
    });
}
