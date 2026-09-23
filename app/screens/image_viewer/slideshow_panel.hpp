/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstdint>
#include <functional>
#include "lvgl.h"

void image_slideshow_panel_build(lv_obj_t *root, uint32_t interval_s,
                                 std::function<void(uint32_t interval_s)> on_interval,
                                 std::function<void()> on_start, std::function<void()> on_close);
