#pragma once
#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    AVI_DMUX_AUDIO_CODEC_UNKNOWN,
    AVI_DMUX_AUDIO_CODEC_PCM,
    AVI_DMUX_AUDIO_CODEC_MP3,
} avi_dmux_audio_codec_t;

typedef enum {
    AVI_DMUX_VIDEO_CODEC_UNKNOWN,
    AVI_DMUX_VIDEO_CODEC_MJPEG,
} avi_dmux_video_codec_t;

typedef struct {
    struct {
        avi_dmux_audio_codec_t codec;
        uint8_t channels;
        uint8_t bits_per_sample;
        uint32_t sampling_rate;
        uint32_t max_frame_size;
    } audio;
    struct {
        avi_dmux_video_codec_t codec;
        uint32_t width;
        uint32_t height;
        uint32_t total_frames;
        uint32_t frame_rate;  // micro seconds per frame
        uint32_t max_frame_size;
    } video;
    off_t movi_location;
} avi_dmux_info_t;

typedef enum {
    AVI_DMUX_FRAME_TYPE_VIDEO,
    AVI_DMUX_FRAME_TYPE_AUDIO,
} avi_dmux_frame_type_t;

typedef struct {
    avi_dmux_frame_type_t type;
    uint32_t size;
    uint32_t frame_index;  // For video frames
} avi_dmux_frame_t;

typedef struct avi_dmux avi_dmux_t;
avi_dmux_t *avi_dmux_create(const char *file);
void avi_dmux_delete(avi_dmux_t *dmux);
avi_dmux_info_t *avi_dmux_parse_info(avi_dmux_t *dmux);
bool avi_dmux_read_frame(avi_dmux_t *dmux, avi_dmux_frame_t *frame,
                         uint8_t *video_buffer, uint32_t video_buffer_size,
                         uint8_t *audio_buffer, uint32_t audio_buffer_size);
void avi_dmux_seek_to_start(avi_dmux_t *dmux);
