/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "bsp.h"

inline constexpr int kMinDisplayBrightness = 1;

void settings_init();
void settings_apply();
void settings_commit();

int settings_display_brightness();
void settings_set_display_brightness(int percent);

bsp_pixel_format_t settings_display_pixel_format();
esp_err_t settings_set_display_pixel_format(bsp_pixel_format_t format);
