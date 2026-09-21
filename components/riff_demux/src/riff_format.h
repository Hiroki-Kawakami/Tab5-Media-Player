/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdint.h>

#define RIFF_FOURCC(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

enum {
    RIFF_ID_RIFF = RIFF_FOURCC('R', 'I', 'F', 'F'),
    RIFF_ID_LIST = RIFF_FOURCC('L', 'I', 'S', 'T'),
    RIFF_ID_fmt  = RIFF_FOURCC('f', 'm', 't', ' '),
    RIFF_ID_data = RIFF_FOURCC('d', 'a', 't', 'a'),
    RIFF_TYPE_AVI = RIFF_FOURCC('A', 'V', 'I', ' '),
    RIFF_TYPE_WAVE = RIFF_FOURCC('W', 'A', 'V', 'E'),
};

enum {
    AVI_hdrl = RIFF_FOURCC('h', 'd', 'r', 'l'),
    AVI_strl = RIFF_FOURCC('s', 't', 'r', 'l'),
    AVI_movi = RIFF_FOURCC('m', 'o', 'v', 'i'),
    AVI_avih = RIFF_FOURCC('a', 'v', 'i', 'h'),
    AVI_strh = RIFF_FOURCC('s', 't', 'r', 'h'),
    AVI_strf = RIFF_FOURCC('s', 't', 'r', 'f'),
    AVI_idx1 = RIFF_FOURCC('i', 'd', 'x', '1'),
    AVI_vids = RIFF_FOURCC('v', 'i', 'd', 's'),
    AVI_auds = RIFF_FOURCC('a', 'u', 'd', 's'),
    AVI_00db = RIFF_FOURCC('0', '0', 'd', 'b'),
    AVI_00dc = RIFF_FOURCC('0', '0', 'd', 'c'),
    AVI_01wb = RIFF_FOURCC('0', '1', 'w', 'b'),
    AVI_MJPG = RIFF_FOURCC('M', 'J', 'P', 'G'),
    AVI_mjpg = RIFF_FOURCC('m', 'j', 'p', 'g'),
    AVI_jpeg = RIFF_FOURCC('j', 'p', 'e', 'g'),
    AVI_JPEG = RIFF_FOURCC('J', 'P', 'E', 'G'),
    AVI_H264 = RIFF_FOURCC('H', '2', '6', '4'),
    AVI_h264 = RIFF_FOURCC('h', '2', '6', '4'),
    AVI_X264 = RIFF_FOURCC('X', '2', '6', '4'),
    AVI_x264 = RIFF_FOURCC('x', '2', '6', '4'),
    AVI_AVC1 = RIFF_FOURCC('A', 'V', 'C', '1'),
    AVI_avc1 = RIFF_FOURCC('a', 'v', 'c', '1'),
    AVI_DAVC = RIFF_FOURCC('D', 'A', 'V', 'C'),
    AVI_mpg2 = RIFF_FOURCC('m', 'p', 'g', '2'),
    AVI_MPG2 = RIFF_FOURCC('M', 'P', 'G', '2'),
    AVI_MPEG = RIFF_FOURCC('M', 'P', 'E', 'G'),
};

#define RIFF_WAVE_FORMAT_PCM 0x0001
#define RIFF_WAVE_FORMAT_MP3 0x0055
#define RIFF_WAVE_FORMAT_IMA_ADPCM 0x0011
#define RIFF_WAVE_FORMAT_AAC 0x00FF
#define RIFF_WAVE_FORMAT_AAC_ADTS 0x1600
#define RIFF_WAVE_FORMAT_AAC_FAAD 0x706D
#define RIFF_WAVE_FORMAT_EXTENSIBLE 0xFFFE
#define RIFF_WAVE_EXTRA_LIMIT 256
#define RIFF_BITMAP_EXTRA_LIMIT 4096
#define AVI_INDEX_KEYFRAME 0x10

typedef struct {
    uint32_t fourcc;
    uint32_t size;
} __attribute__((packed)) riff_chunk_t;

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
} __attribute__((packed)) riff_wave_format_t;

typedef struct {
    uint32_t chunk_id;
    uint32_t flags;
    uint32_t offset;
    uint32_t size;
} __attribute__((packed)) avi_index_entry_t;
