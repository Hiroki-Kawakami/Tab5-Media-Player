/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "render_target.hpp"
#include "media_player.hpp"
#include "lvgl.h"

bool video_presenter_begin(const SharedSram &sram, bsp_rotation_t rotation);
void video_presenter_end();

bool video_presenter_submit(const uint8_t *data, std::size_t len,
                            VideoPresenterRelease release, void *ctx);
void video_presenter_flush();

void video_presenter_set_overlay(lv_display_t *overlay);
void video_presenter_set_rotation(bsp_rotation_t rotation);
void video_presenter_set_source_rotation(bsp_rotation_t rotation);
void video_presenter_mark_overlay_dirty();
void video_presenter_repaint();

float video_presenter_fps();
std::string video_presenter_error();
