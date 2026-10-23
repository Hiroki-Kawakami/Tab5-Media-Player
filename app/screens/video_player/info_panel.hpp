/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <functional>
#include <string>
#include "lvgl.h"
#include "playback/player.hpp"
#include "screens/home/settings_widgets.hpp"

void player_info_panel_build(lv_obj_t *root, const std::string &name, const MediaSummary &summary,
                             std::function<void()> on_close);

/* The sections alone, for a page the caller styles; uncached, and the Video
 * section only when the file has a video track. Updates in place as
 * panel_sections_show() does. */
void player_info_sections_show(lv_obj_t *contents, const std::string &name,
                               const MediaSummary &summary, const SettingColors *colors);
