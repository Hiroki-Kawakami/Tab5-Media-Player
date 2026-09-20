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
#include "media/media_types.hpp"

bool video_presenter_begin(const SharedSram &sram, bsp_rotation_t rotation);
void video_presenter_end();

bool video_presenter_open_stream(const TrackInfo &track, std::string *error);

bool video_presenter_pipelined();
bool video_presenter_needs_source();
bool video_presenter_submit(const uint8_t *data, std::size_t len,
                            VideoPresenterRelease release, void *ctx, bool present,
                            int64_t due_us);
void video_presenter_flush();
void video_presenter_drain();

struct VideoInsets {
    int top = 0;
    int bottom = 0;
    int left = 0;
    int right = 0;
};

void video_presenter_set_ui_insets(const VideoInsets &insets);
void video_presenter_set_rotation(bsp_rotation_t rotation);
void video_presenter_set_source_rotation(bsp_rotation_t rotation);

float video_presenter_fps();
std::string video_presenter_error();
