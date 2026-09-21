/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "media_buffer.h"
#include "media_tags.h"
#include "riff_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    AVI_VIDEO_CODEC_NONE,
    AVI_VIDEO_CODEC_MJPEG,
    AVI_VIDEO_CODEC_H264,
    AVI_VIDEO_CODEC_MPEG2,
    AVI_VIDEO_CODEC_UNSUPPORTED,
} avi_video_codec_t;

typedef struct {
    struct {
        avi_video_codec_t codec;
        uint32_t width;
        uint32_t height;
        uint32_t frame_count;
        uint32_t frame_interval_us;
        uint32_t max_frame_bytes;
        const uint8_t *codec_private;
        uint32_t codec_private_size;
    } video;
    struct {
        riff_audio_codec_t codec;
        uint32_t sample_rate;
        uint32_t bitrate_bps;
        uint32_t max_frame_bytes;
        uint8_t channels;
        uint8_t bits_per_sample;
        uint16_t block_align;
        const uint8_t *codec_private;
        uint32_t codec_private_size;
    } audio;
    bool seekable;
    media_tags_t tags;
} avi_info_t;

typedef enum {
    AVI_PACKET_VIDEO,
    AVI_PACKET_AUDIO,
} avi_packet_type_t;

typedef struct {
    avi_packet_type_t type;
    const uint8_t *data;
    uint32_t size;
    uint32_t ref;
    uint32_t frame_index;
    bool keyframe;
} avi_packet_t;

typedef struct avi_demux avi_demux_t;

avi_demux_t *avi_demux_open(const char *path, const media_arena_t *arena, const char **error);
void avi_demux_close(avi_demux_t *demux);

const avi_info_t *avi_demux_info(const avi_demux_t *demux);

media_buffer_t *avi_demux_buffer(avi_demux_t *demux);

bool avi_demux_read(avi_demux_t *demux, avi_packet_t *packet, bool want_audio);

bool avi_demux_seek(avi_demux_t *demux, uint32_t frame, uint32_t *landed_frame);
bool avi_demux_keyframe_before(const avi_demux_t *demux, uint32_t frame, uint32_t *key_frame);

#ifdef __cplusplus
}
#endif
