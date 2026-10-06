/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "lvgl.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>

void media_format_time(char *text, std::size_t size, int64_t seconds);

lv_obj_t *media_icon_button(lv_obj_t *parent, int32_t size, const lv_font_t *font, const char *icon,
                            lv_color_t foreground, lv_obj_t **label = nullptr);
lv_obj_t *media_toggle_button(lv_obj_t *parent, int32_t size, const lv_font_t *font,
                              const char *icon, lv_color_t foreground, lv_color_t background,
                              lv_obj_t **label = nullptr);
lv_obj_t *media_slider(lv_obj_t *parent, int32_t max, lv_color_t foreground, lv_color_t track);

struct MediaTopBar {
    lv_obj_t *title;
    lv_obj_t *info_button;
};

/* Without on_info there is no info button. */
MediaTopBar media_top_bar_build(lv_obj_t *bar, const char *title, std::function<void()> on_back,
                                std::function<void()> on_info);
/* Goes at the bar's right end; fit the title again after adding one. */
lv_obj_t *media_top_bar_text_button(lv_obj_t *bar, const char *text, std::function<void()> on_click,
                                    lv_obj_t **label = nullptr);
/* Shortens the title to the room the bar's other items leave it. */
void media_top_bar_fit_title(lv_obj_t *bar, lv_obj_t *title);

const char *media_volume_icon(int32_t volume);
void media_volume_bind(lv_obj_t *mute_button, lv_obj_t *icon_label, lv_obj_t *slider);
void media_volume_show(lv_obj_t *icon_label, lv_obj_t *slider, int32_t volume);
