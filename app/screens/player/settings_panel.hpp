/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <functional>
#include "lvgl.h"

/* Display and Sound settings on a dark panel, for use over the video. The root
 * takes LV_EVENT_REFRESH to re-read the values it shares with the player bar. */
void player_settings_panel_build(lv_obj_t *root, std::function<void()> on_close);
