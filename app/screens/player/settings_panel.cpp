/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "settings_panel.hpp"
#include "resources.h"
#include "screens/home/settings_widgets.hpp"
#include "settings.hpp"
#include "widgets.hpp"

static constexpr SettingColors kColors = {
    .page_bg    = 0x101010,
    .section_bg = 0x1e1e1e,
    .value      = 0x9e9e9e,
    .separator  = 0x3a3a3a,
    .track      = 0x404040,
    .accent     = 0x2196f3,
};

static constexpr int32_t kPadding = 24;
static constexpr int32_t kTitleIndent = 48;
static constexpr int32_t kCloseButton = 56;
static constexpr int32_t kHeaderHeight = kPadding + kCloseButton;

static void set_brightness_text(lv_obj_t *value, int brightness) {
    lv_label_set_text_fmt(value, "%d%%", brightness);
}

static void set_volume_text(lv_obj_t *value, int volume) {
    lv_label_set_text_fmt(value, "%s  %d",
                          settings_volume_is_headphone() ? "Headphone" : "Speaker", volume);
}

static void build_header(lv_obj_t *root, std::function<void()> on_close) {
    auto header = lv_container_create(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(header, lv_pct(100), kHeaderHeight);
    lv_obj_set_style_pad_left(header, kTitleIndent, 0);
    lv_obj_set_style_pad_right(header, kPadding, 0);
    lv_obj_set_style_pad_top(header, kPadding, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kColors.page_bg), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);

    auto title = lv_label_create(header);
    lv_obj_set_flex_grow(title, 1);
    lv_obj_set_style_text_font(title, lv_widgets_title_font(), 0);
    lv_label_set_text(title, "Settings");

    auto close = lv_button_create(header, LV_BUTTON_STYLE_PLAIN);
    lv_obj_set_size(close, kCloseButton, kCloseButton);
    lv_obj_set_style_pad_all(close, 0, 0);
    lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(close, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(close, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(close, lv_color_white(), 0);
    lv_button_set_text(close, TABLER_X, &icon_36);
    lv_obj_add_event_fn(close, LV_EVENT_CLICKED, [on_close](lv_event_t *) { on_close(); });
}

void player_settings_panel_build(lv_obj_t *root, std::function<void()> on_close) {
    lv_obj_set_style_bg_color(root, lv_color_hex(kColors.page_bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(root, lv_color_white(), 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);

    build_header(root, std::move(on_close));

    auto contents = lv_spacer_create(root, lv_pct(100), LV_SIZE_CONTENT, 1);
    lv_obj_set_flex_flow(contents, LV_FLEX_FLOW_COLUMN);
    lv_setting_page_style(contents, &kColors);

    auto section = lv_setting_section_create(contents, "Display", &kColors);

    auto row = lv_setting_row_create(section, "Brightness");
    auto brightness_value = lv_setting_value_create(row, &kColors);
    set_brightness_text(brightness_value, settings_display_brightness());

    auto brightness = lv_setting_slider_create(section, kMinDisplayBrightness, 100,
                                               settings_display_brightness(), &kColors);
    lv_obj_add_event_fn(brightness, LV_EVENT_VALUE_CHANGED,
                        [brightness, brightness_value](lv_event_t *) {
        const int percent = lv_slider_get_value(brightness);
        settings_set_display_brightness(percent);
        set_brightness_text(brightness_value, percent);
    });
    lv_obj_add_event_fn(brightness, LV_EVENT_RELEASED, [](lv_event_t *) { settings_commit(); });

    lv_setting_separator_create(section, &kColors);

    row = lv_setting_row_create(section, "Rotation Lock");
    auto rotation_lock = lv_setting_switch_create(row, settings_rotation_locked(),
                                                  [](lv_obj_t *, bool locked) {
        settings_set_rotation_lock(locked);
        settings_commit();
    }, &kColors);

    section = lv_setting_section_create(contents, "Sound", &kColors);

    row = lv_setting_row_create(section, "Volume");
    auto volume_value = lv_setting_value_create(row, &kColors);
    set_volume_text(volume_value, settings_volume());

    auto volume = lv_setting_slider_create(section, 0, 100, settings_volume(), &kColors);
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

    lv_setting_separator_create(section, &kColors);

    row = lv_setting_row_create(section, "Equalizer");
    auto equalizer = lv_setting_switch_create(row, settings_equalizer_enabled(),
                                              [](lv_obj_t *, bool enabled) {
        settings_set_equalizer_enabled(enabled);
        settings_commit();
    }, &kColors);

    lv_obj_add_event_fn(root, LV_EVENT_REFRESH, [=](lv_event_t *) {
        lv_slider_set_value(brightness, settings_display_brightness(), LV_ANIM_OFF);
        set_brightness_text(brightness_value, settings_display_brightness());
        lv_obj_set_state(rotation_lock, LV_STATE_CHECKED, settings_rotation_locked());
        lv_slider_set_value(volume, settings_volume(), LV_ANIM_OFF);
        set_volume_text(volume_value, settings_volume());
        lv_obj_set_state(equalizer, LV_STATE_CHECKED, settings_equalizer_enabled());
    });
}
