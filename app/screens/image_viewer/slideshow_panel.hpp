/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include "lvgl.h"
#include "slideshow/transition.hpp"

struct SlideshowPanelValues {
    uint32_t interval_s;
    bool shuffle;
    TransitionKind transition;
    TransitionDirection direction;
    bool bgm;
    bool bgm_shuffle;
    std::string bgm_path;
};

/* The root takes LV_EVENT_REFRESH with a `const SlideshowPanelValues *` to
   show a BGM source chosen elsewhere. */
void image_slideshow_panel_build(lv_obj_t *root, const SlideshowPanelValues &values,
                                 std::function<void(const SlideshowPanelValues &)> on_change,
                                 std::function<void()> on_choose_bgm,
                                 std::function<void()> on_start, std::function<void()> on_close);
