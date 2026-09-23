/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <functional>
#include "lvgl.h"

void image_slideshow_panel_build(lv_obj_t *root, std::function<void()> on_start,
                                 std::function<void()> on_close);
