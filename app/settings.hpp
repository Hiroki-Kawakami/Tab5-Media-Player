/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once

inline constexpr int kMinDisplayBrightness = 1;

void settings_init();
void settings_commit();

int settings_display_brightness();
void settings_set_display_brightness(int percent);
