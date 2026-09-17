/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/media_types.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

void audio_out_start();
bool audio_out_open(const TrackInfo &track, std::string *note);
void audio_out_close();
void audio_out_write(const uint8_t *data, std::size_t len);
void audio_out_flush();
uint64_t audio_out_position_us();
bool audio_out_running();
void audio_out_set_volume(int volume);
int audio_out_get_volume();
