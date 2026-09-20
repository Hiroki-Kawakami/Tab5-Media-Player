/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "settings.hpp"
#include "bsp.h"

static int s_display_brightness = 80;

void settings_init() {
    bsp_display_set_brightness(s_display_brightness);
}

int settings_display_brightness() {
    return s_display_brightness;
}

void settings_set_display_brightness(int percent) {
    if (percent < kMinDisplayBrightness) percent = kMinDisplayBrightness;
    if (percent > 100) percent = 100;
    s_display_brightness = percent;
    bsp_display_set_brightness(percent);
}
