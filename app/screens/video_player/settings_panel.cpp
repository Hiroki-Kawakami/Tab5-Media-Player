/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "settings_panel.hpp"
#include "panel.hpp"
#include "screens/home/settings_widgets.hpp"
#include "settings.hpp"
#include "widgets.hpp"

static void set_brightness_text(lv_obj_t *value, int brightness) {
    lv_label_set_text_fmt(value, "%d%%", brightness);
}

static void set_volume_text(lv_obj_t *value, int volume) {
    lv_label_set_text_fmt(value, "%s  %d",
                          settings_volume_is_headphone() ? "Headphone" : "Speaker", volume);
}

void player_settings_panel_build(lv_obj_t *root, std::function<void()> on_close) {
    auto contents = player_panel_build(root, "Settings", std::move(on_close));

    auto section = lv_setting_section_create(contents, "Display", &kPanelColors);

    auto row = lv_setting_row_create(section, "Brightness");
    auto brightness_value = lv_setting_value_create(row, &kPanelColors);
    set_brightness_text(brightness_value, settings_display_brightness());

    auto brightness = lv_setting_slider_create(section, kMinDisplayBrightness, 100,
                                               settings_display_brightness(), &kPanelColors);
    lv_obj_add_event_fn(brightness, LV_EVENT_VALUE_CHANGED,
                        [brightness, brightness_value](lv_event_t *) {
        const int percent = lv_slider_get_value(brightness);
        settings_set_display_brightness(percent);
        set_brightness_text(brightness_value, percent);
    });
    lv_obj_add_event_fn(brightness, LV_EVENT_RELEASED, [](lv_event_t *) { settings_commit(); });

    lv_setting_separator_create(section, &kPanelColors);

    row = lv_setting_row_create(section, "Rotation Lock");
    auto rotation_lock = lv_setting_switch_create(row, settings_rotation_locked(),
                                                  [](lv_obj_t *, bool locked) {
        settings_set_rotation_lock(locked);
        settings_commit();
    }, &kPanelColors);

    section = lv_setting_section_create(contents, "Sound", &kPanelColors);

    row = lv_setting_row_create(section, "Volume");
    auto volume_value = lv_setting_value_create(row, &kPanelColors);
    set_volume_text(volume_value, settings_volume());

    auto volume = lv_setting_slider_create(section, 0, 100, settings_volume(), &kPanelColors);
    lv_obj_add_event_fn(volume, LV_EVENT_VALUE_CHANGED, [volume, volume_value](lv_event_t *) {
        const int percent = lv_slider_get_value(volume);
        bsp_audio_set_mute(false);
        settings_set_volume(percent);
        set_volume_text(volume_value, percent);
    });
    lv_obj_add_event_fn(volume, LV_EVENT_RELEASED, [](lv_event_t *) { settings_commit(); });
    settings_volume_observe(volume, [volume, volume_value](int percent) {
        if (lv_obj_has_state(volume, LV_STATE_PRESSED)) return;
        lv_slider_set_value(volume, percent, LV_ANIM_OFF);
        set_volume_text(volume_value, percent);
    });

    lv_setting_separator_create(section, &kPanelColors);

    row = lv_setting_row_create(section, "Equalizer");
    auto equalizer = lv_setting_switch_create(row, settings_equalizer_enabled(),
                                              [](lv_obj_t *, bool enabled) {
        settings_set_equalizer_enabled(enabled);
        settings_commit();
    }, &kPanelColors);

    lv_obj_add_event_fn(root, LV_EVENT_REFRESH, [=](lv_event_t *) {
        lv_slider_set_value(brightness, settings_display_brightness(), LV_ANIM_OFF);
        set_brightness_text(brightness_value, settings_display_brightness());
        lv_obj_set_state(rotation_lock, LV_STATE_CHECKED, settings_rotation_locked());
        lv_slider_set_value(volume, settings_volume(), LV_ANIM_OFF);
        set_volume_text(volume_value, settings_volume());
        lv_obj_set_state(equalizer, LV_STATE_CHECKED, settings_equalizer_enabled());
    });
}
