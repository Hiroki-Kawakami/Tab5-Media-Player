/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_buffer.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    ES_AUDIO_CODEC_NONE,
    ES_AUDIO_CODEC_MP3,
    ES_AUDIO_CODEC_AAC,
} es_audio_codec_t;

typedef struct {
    es_audio_codec_t codec;
    uint32_t sample_rate;
    uint32_t bitrate_bps;
    uint8_t channels;
    int64_t duration_us;
    bool duration_exact;
} es_audio_info_t;

typedef struct {
    int64_t pts_us;
    const uint8_t *data;
    uint32_t size;
    uint32_t ref;
} es_audio_packet_t;

typedef struct es_audio_demux es_audio_demux_t;

es_audio_demux_t *es_audio_demux_open(const char *path, const media_arena_t *arena,
                                      const char **error);
void es_audio_demux_close(es_audio_demux_t *demux);

const es_audio_info_t *es_audio_demux_info(const es_audio_demux_t *demux);
media_buffer_t *es_audio_demux_buffer(es_audio_demux_t *demux);

bool es_audio_demux_read(es_audio_demux_t *demux, es_audio_packet_t *packet);
bool es_audio_demux_seek(es_audio_demux_t *demux, int64_t pts_us, int64_t *landed_us);

#ifdef __cplusplus
}
#endif
