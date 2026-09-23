/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "slideshow_panel.hpp"
#include "screens/player_panel.hpp"
#include "widgets.hpp"

static constexpr int32_t kStartButtonHeight = 72;

void image_slideshow_panel_build(lv_obj_t *root, std::function<void()> on_start,
                                 std::function<void()> on_close) {
    auto contents = player_panel_build(root, "Slideshow", std::move(on_close));

    auto start = lv_button_create(contents, LV_BUTTON_STYLE_PRIMARY);
    lv_obj_set_size(start, lv_pct(100), kStartButtonHeight);
    lv_button_set_text(start, "Start Slideshow");
    lv_obj_add_event_fn(start, LV_EVENT_CLICKED, [on_start](lv_event_t *) { on_start(); });
}
