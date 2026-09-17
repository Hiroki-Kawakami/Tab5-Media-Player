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
    MKV_VIDEO_CODEC_NONE,
    MKV_VIDEO_CODEC_MJPEG,
    MKV_VIDEO_CODEC_UNSUPPORTED,
} mkv_video_codec_t;

typedef enum {
    MKV_AUDIO_CODEC_NONE,
    MKV_AUDIO_CODEC_PCM,
    MKV_AUDIO_CODEC_MP3,
    MKV_AUDIO_CODEC_UNSUPPORTED,
} mkv_audio_codec_t;

typedef struct {
    struct {
        mkv_video_codec_t codec;
        uint32_t width;
        uint32_t height;
        int64_t frame_interval_us;
        uint16_t rotation_ccw;
    } video;
    struct {
        mkv_audio_codec_t codec;
        uint32_t sample_rate;
        uint8_t channels;
        uint8_t bits_per_sample;
    } audio;
    int64_t duration_us;
    bool seekable;
} mkv_info_t;

typedef enum {
    MKV_PACKET_VIDEO,
    MKV_PACKET_AUDIO,
} mkv_packet_type_t;

typedef struct {
    mkv_packet_type_t type;
    int64_t pts_us;
    bool keyframe;
    const uint8_t *data;
    uint32_t size;
    uint32_t ref;
} mkv_packet_t;

typedef struct mkv_demux mkv_demux_t;

mkv_demux_t *mkv_demux_open(const char *path, const media_arena_t *arena, const char **error);
void mkv_demux_close(mkv_demux_t *demux);

const mkv_info_t *mkv_demux_info(const mkv_demux_t *demux);
media_buffer_t *mkv_demux_buffer(mkv_demux_t *demux);

bool mkv_demux_read(mkv_demux_t *demux, mkv_packet_t *packet, bool want_audio);
bool mkv_demux_seek(mkv_demux_t *demux, int64_t pts_us);

#ifdef __cplusplus
}
#endif
