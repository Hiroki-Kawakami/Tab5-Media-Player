/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstdint>
#include <functional>
#include "lvgl.h"
#include "slideshow/transition.hpp"

struct SlideshowPanelValues {
    uint32_t interval_s;
    TransitionKind transition;
    TransitionDirection direction;
};

void image_slideshow_panel_build(lv_obj_t *root, const SlideshowPanelValues &values,
                                 std::function<void(const SlideshowPanelValues &)> on_change,
                                 std::function<void()> on_start, std::function<void()> on_close);
