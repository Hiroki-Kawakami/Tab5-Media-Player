/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "audio_output.hpp"
#include "media/media_types.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

void audio_decoder_start();
bool audio_decoder_open(const TrackInfo &track, bool aac_sbr, AudioContent content,
                        std::string *note);
void audio_decoder_close();
void audio_decoder_write(const uint8_t *data, std::size_t len);
void audio_decoder_flush();
uint64_t audio_decoder_position_us();
bool audio_decoder_running();
