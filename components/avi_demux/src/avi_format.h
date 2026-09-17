/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdint.h>

#define AVI_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

enum {
    AVI_RIFF = AVI_FOURCC('R', 'I', 'F', 'F'),
    AVI_TYPE = AVI_FOURCC('A', 'V', 'I', ' '),
    AVI_LIST = AVI_FOURCC('L', 'I', 'S', 'T'),
    AVI_hdrl = AVI_FOURCC('h', 'd', 'r', 'l'),
    AVI_strl = AVI_FOURCC('s', 't', 'r', 'l'),
    AVI_movi = AVI_FOURCC('m', 'o', 'v', 'i'),
    AVI_avih = AVI_FOURCC('a', 'v', 'i', 'h'),
    AVI_strh = AVI_FOURCC('s', 't', 'r', 'h'),
    AVI_strf = AVI_FOURCC('s', 't', 'r', 'f'),
    AVI_idx1 = AVI_FOURCC('i', 'd', 'x', '1'),
    AVI_vids = AVI_FOURCC('v', 'i', 'd', 's'),
    AVI_auds = AVI_FOURCC('a', 'u', 'd', 's'),
    AVI_00db = AVI_FOURCC('0', '0', 'd', 'b'),
    AVI_00dc = AVI_FOURCC('0', '0', 'd', 'c'),
    AVI_01wb = AVI_FOURCC('0', '1', 'w', 'b'),
    AVI_MJPG = AVI_FOURCC('M', 'J', 'P', 'G'),
    AVI_mjpg = AVI_FOURCC('m', 'j', 'p', 'g'),
    AVI_jpeg = AVI_FOURCC('j', 'p', 'e', 'g'),
    AVI_JPEG = AVI_FOURCC('J', 'P', 'E', 'G'),
    AVI_H264 = AVI_FOURCC('H', '2', '6', '4'),
    AVI_h264 = AVI_FOURCC('h', '2', '6', '4'),
    AVI_X264 = AVI_FOURCC('X', '2', '6', '4'),
    AVI_x264 = AVI_FOURCC('x', '2', '6', '4'),
    AVI_AVC1 = AVI_FOURCC('A', 'V', 'C', '1'),
    AVI_avc1 = AVI_FOURCC('a', 'v', 'c', '1'),
    AVI_DAVC = AVI_FOURCC('D', 'A', 'V', 'C'),
};

#define AVI_WAVE_FORMAT_PCM 0x0001
#define AVI_WAVE_FORMAT_MP3 0x0055
#define AVI_WAVE_FORMAT_IMA_ADPCM 0x0011
#define AVI_WAVE_FORMAT_AAC 0x00FF
#define AVI_WAVE_FORMAT_AAC_ADTS 0x1600
#define AVI_WAVE_FORMAT_AAC_FAAD 0x706D
#define AVI_WAVE_FORMAT_EXTRA_LIMIT 256
#define AVI_BITMAP_EXTRA_LIMIT 4096
#define AVI_INDEX_KEYFRAME 0x10

typedef struct {
    uint32_t fourcc;
    uint32_t size;
} __attribute__((packed)) avi_chunk_t;

typedef struct {
    uint32_t micro_sec_per_frame;
    uint32_t max_bytes_per_sec;
    uint32_t padding_granularity;
    uint32_t flags;
    uint32_t total_frames;
    uint32_t initial_frames;
    uint32_t streams;
    uint32_t suggested_buffer_size;
    uint32_t width;
    uint32_t height;
    uint32_t reserved[4];
} __attribute__((packed)) avi_main_header_t;

typedef struct {
    uint32_t fourcc_type;
    uint32_t fourcc_handler;
    uint32_t flags;
    uint16_t priority;
    uint16_t language;
    uint32_t initial_frames;
    uint32_t scale;
    uint32_t rate;
    uint32_t start;
    uint32_t length;
    uint32_t suggested_buffer_size;
    uint32_t quality;
    uint32_t sample_size;
    int16_t frame_left;
    int16_t frame_top;
    int16_t frame_right;
    int16_t frame_bottom;
} __attribute__((packed)) avi_stream_header_t;

typedef struct {
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint16_t planes;
    uint16_t bit_count;
    uint32_t compression;
    uint32_t size_image;
    uint32_t x_pels_per_meter;
    uint32_t y_pels_per_meter;
    uint32_t clr_used;
    uint32_t clr_important;
} __attribute__((packed)) avi_bitmap_info_t;

typedef struct {
    uint16_t format_tag;
    uint16_t channels;
    uint32_t samples_per_sec;
    uint32_t avg_bytes_per_sec;
    uint16_t block_align;
    uint16_t bits_per_sample;
} __attribute__((packed)) avi_wave_format_t;

typedef struct {
    uint32_t chunk_id;
    uint32_t flags;
    uint32_t offset;
    uint32_t size;
} __attribute__((packed)) avi_index_entry_t;
