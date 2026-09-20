/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <functional>
#include <string>
#include "lvgl.h"
#include "playback/player.hpp"

void player_info_panel_build(lv_obj_t *root, const std::string &name, const MediaSummary &summary,
                             std::function<void()> on_close,
                             std::function<void(bool)> on_scroll);
