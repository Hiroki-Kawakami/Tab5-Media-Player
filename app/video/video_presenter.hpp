/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

#include "bsp_types.h"
#include "lvgl.h"

typedef void (*VideoPresenterRelease)(void *ctx);

bool video_presenter_begin(bsp_rotation_t rotation, lv_display_t *overlay);
void video_presenter_end();

bool video_presenter_submit(const uint8_t *data, std::size_t len,
                            VideoPresenterRelease release, void *ctx);
void video_presenter_flush();

void video_presenter_mark_overlay_dirty();
void video_presenter_repaint();

float video_presenter_fps();
std::string video_presenter_error();
