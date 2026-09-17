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
    MP4_VIDEO_CODEC_NONE,
    MP4_VIDEO_CODEC_MJPEG,
    MP4_VIDEO_CODEC_H264,
    MP4_VIDEO_CODEC_UNSUPPORTED,
} mp4_video_codec_t;

typedef enum {
    MP4_AUDIO_CODEC_NONE,
    MP4_AUDIO_CODEC_PCM,
    MP4_AUDIO_CODEC_MP3,
    MP4_AUDIO_CODEC_AAC,
    MP4_AUDIO_CODEC_OPUS,
    MP4_AUDIO_CODEC_UNSUPPORTED,
} mp4_audio_codec_t;

typedef struct {
    struct {
        mp4_video_codec_t codec;
        uint32_t width;
        uint32_t height;
        int64_t frame_interval_us;
        uint16_t rotation_ccw;
        const uint8_t *codec_private;
        uint32_t codec_private_size;
    } video;
    struct {
        mp4_audio_codec_t codec;
        uint32_t sample_rate;
        uint8_t channels;
        uint8_t bits_per_sample;
        const uint8_t *codec_private;
        uint32_t codec_private_size;
    } audio;
    int64_t duration_us;
    bool seekable;
} mp4_info_t;

typedef enum {
    MP4_PACKET_VIDEO,
    MP4_PACKET_AUDIO,
} mp4_packet_type_t;

typedef struct {
    mp4_packet_type_t type;
    int64_t pts_us;
    bool keyframe;
    const uint8_t *data;
    uint32_t size;
    uint32_t ref;
} mp4_packet_t;

typedef struct mp4_demux mp4_demux_t;

mp4_demux_t *mp4_demux_open(const char *path, const media_arena_t *arena, const char **error);
void mp4_demux_close(mp4_demux_t *demux);

const mp4_info_t *mp4_demux_info(const mp4_demux_t *demux);
media_buffer_t *mp4_demux_buffer(mp4_demux_t *demux);

bool mp4_demux_read(mp4_demux_t *demux, mp4_packet_t *packet, bool want_audio);
bool mp4_demux_seek(mp4_demux_t *demux, int64_t pts_us, int64_t *landed_us);

#ifdef __cplusplus
}
#endif
