/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "audf_aac.h"
#include "audio_output.hpp"
#include "media/media_types.hpp"
#include <cstddef>
#include <cstdint>
#include <string>

void audio_decoder_start();
bool audio_decoder_open(const TrackInfo &track, audf_aac_he_t aac_he, AudioContent content,
                        std::string *note);
void audio_decoder_close();
void audio_decoder_write(const uint8_t *data, std::size_t len);
/* How far before a seek target decoding has to start for the codec to
   settle: 80 ms for Opus, 0 otherwise. */
int64_t audio_decoder_preroll_us();
/* Drops what the decoder carries from one packet to the next, as for a seek,
   and the next discard_us of output; start also drops the codec's leading
   samples, for a read from the beginning. */
void audio_decoder_flush(int64_t discard_us, bool start);
uint64_t audio_decoder_position_us();
bool audio_decoder_running();
