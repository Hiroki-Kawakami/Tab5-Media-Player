/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <functional>
#include "bsp.h"
#include "lvgl.h"

inline constexpr int kMinDisplayBrightness = 1;
inline constexpr int kDefaultSpeakerVolume = 60;
inline constexpr int kDefaultHeadphoneVolume = 40;

void settings_init();
void settings_apply();
void settings_commit();

int settings_display_brightness();
void settings_set_display_brightness(int percent);

bsp_pixel_format_t settings_display_pixel_format();
esp_err_t settings_set_display_pixel_format(bsp_pixel_format_t format);

bool settings_rotation_locked();
bsp_rotation_t settings_locked_rotation();
void settings_set_rotation_lock(bool locked);

bool settings_volume_is_headphone();
int settings_volume();
void settings_set_volume(int percent);
void settings_volume_observe(lv_obj_t *owner, std::function<void(int)> on_change);

bool settings_equalizer_enabled();
void settings_set_equalizer_enabled(bool enabled);
