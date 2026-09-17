/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "bsp_types.h"
#include "lvgl.h"

using OrientationListener = void (*)(bsp_rotation_t rotation, void *arg);

void ui_orientation_start(lv_display_t *main_display);
bsp_rotation_t ui_orientation_current();
void ui_orientation_set_listener(OrientationListener listener, void *arg);
