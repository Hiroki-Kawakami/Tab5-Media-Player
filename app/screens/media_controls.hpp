/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "lvgl.hpp"

#include <cstddef>
#include <cstdint>

void media_format_time(char *text, std::size_t size, int64_t seconds);

lv_obj_t *media_icon_button(lv_obj_t *parent, int32_t size, const lv_font_t *font, const char *icon,
                            lv_color_t foreground, lv_obj_t **label = nullptr);
lv_obj_t *media_slider(lv_obj_t *parent, int32_t max, lv_color_t foreground, lv_color_t track);

const char *media_volume_icon(int32_t volume);
void media_volume_bind(lv_obj_t *mute_button, lv_obj_t *icon_label, lv_obj_t *slider);
void media_volume_show(lv_obj_t *icon_label, lv_obj_t *slider, int32_t volume);
