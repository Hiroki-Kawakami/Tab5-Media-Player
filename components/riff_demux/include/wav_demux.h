/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_buffer.h"
#include "riff_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    riff_audio_codec_t codec;
    uint32_t sample_rate;
    uint32_t bitrate_bps;
    uint8_t channels;
    uint8_t bits_per_sample;
    uint16_t block_align;
    const uint8_t *codec_private;
    uint32_t codec_private_size;
    int64_t duration_us;
} wav_info_t;

typedef struct {
    int64_t pts_us;
    const uint8_t *data;
    uint32_t size;
    uint32_t ref;
} wav_packet_t;

typedef struct wav_demux wav_demux_t;

wav_demux_t *wav_demux_open(const char *path, const media_arena_t *arena, const char **error);
void wav_demux_close(wav_demux_t *demux);

const wav_info_t *wav_demux_info(const wav_demux_t *demux);
media_buffer_t *wav_demux_buffer(wav_demux_t *demux);

bool wav_demux_read(wav_demux_t *demux, wav_packet_t *packet);
bool wav_demux_seek(wav_demux_t *demux, int64_t pts_us, int64_t *landed_us);

#ifdef __cplusplus
}
#endif
