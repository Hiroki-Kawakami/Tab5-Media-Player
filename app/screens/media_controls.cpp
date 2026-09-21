/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "media_controls.hpp"
#include "settings.hpp"
#include "bsp.h"
#include "resources.h"
#include "widgets.hpp"

#include <cstdio>
#include <cstring>

void media_format_time(char *text, std::size_t size, int64_t seconds) {
    if (seconds < 0) {
        snprintf(text, size, "--:--");
    } else if (seconds >= 3600) {
        snprintf(text, size, "%d:%02d:%02d", (int)(seconds / 3600), (int)(seconds / 60 % 60),
                 (int)(seconds % 60));
    } else {
        snprintf(text, size, "%d:%02d", (int)(seconds / 60), (int)(seconds % 60));
    }
}

lv_obj_t *media_icon_button(lv_obj_t *parent, int32_t size, const lv_font_t *font, const char *icon,
                            lv_color_t foreground, lv_obj_t **label) {
    lv_obj_t *button = lv_button_create(parent, LV_BUTTON_STYLE_PLAIN);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, foreground, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, foreground, 0);
    lv_obj_set_style_text_color(button, lv_color_hex(0x808080), LV_STATE_DISABLED);
    lv_obj_t *icon_label = lv_button_set_text(button, icon, font);
    if (label) *label = icon_label;
    return button;
}

lv_obj_t *media_slider(lv_obj_t *parent, int32_t max, lv_color_t foreground, lv_color_t track) {
    lv_obj_t *slider = lv_slider_create(parent);
    lv_slider_set_range(slider, 0, max);
    lv_obj_set_height(slider, 8);
    lv_obj_set_style_bg_color(slider, track, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, foreground, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, foreground, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
    return slider;
}

const char *media_volume_icon(int32_t volume) {
    return bsp_audio_get_mute() ? TABLER_VOLUME_3
         : volume <= 0          ? TABLER_VOLUME_4
         : volume < 50          ? TABLER_VOLUME_2
                                : TABLER_VOLUME;
}

static void set_icon(lv_obj_t *icon_label, int32_t volume) {
    if (!icon_label) return;
    const char *icon = media_volume_icon(volume);
    if (strcmp(lv_label_get_text(icon_label), icon) != 0) lv_label_set_text(icon_label, icon);
}

void media_volume_show(lv_obj_t *icon_label, lv_obj_t *slider, int32_t volume) {
    if (slider) lv_slider_set_value(slider, volume, LV_ANIM_OFF);
    set_icon(icon_label, volume);
}

void media_volume_bind(lv_obj_t *mute_button, lv_obj_t *icon_label, lv_obj_t *slider) {
    media_volume_show(icon_label, slider, settings_volume());

    lv_obj_add_event_fn(mute_button, LV_EVENT_CLICKED, [icon_label, slider](lv_event_t *) {
        bsp_audio_set_mute(!bsp_audio_get_mute());
        set_icon(icon_label, lv_slider_get_value(slider));
    });
    lv_obj_add_event_fn(slider, LV_EVENT_VALUE_CHANGED, [icon_label, slider](lv_event_t *) {
        const int32_t volume = lv_slider_get_value(slider);
        bsp_audio_set_mute(false);
        settings_set_volume(volume);
        set_icon(icon_label, volume);
    });
    lv_obj_add_event_fn(slider, LV_EVENT_RELEASED, [](lv_event_t *) { settings_commit(); });
    settings_volume_observe(slider, [icon_label, slider](int volume) {
        if (lv_obj_has_state(slider, LV_STATE_PRESSED)) return;
        media_volume_show(icon_label, slider, volume);
    });
}
