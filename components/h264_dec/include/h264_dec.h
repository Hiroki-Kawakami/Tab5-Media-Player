/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define H264_DEC_WORK_ALIGNMENT 16

typedef enum {
    H264_PROF_MC_LUMA,
    H264_PROF_MC_FETCH,
    H264_PROF_WINDOW,
    H264_PROF_MC_CHROMA,
    H264_PROF_INTRA,
    H264_PROF_CAVLC,
    H264_PROF_IDCT,
    H264_PROF_DEBLOCK,
    H264_PROF_PACK,
    H264_PROF_FLUSH,
    H264_PROF_WAIT,
    H264_PROF_TOTAL,
    H264_PROF_COUNT,
} h264_prof_stage_t;

typedef struct h264_dec h264_dec_t;

typedef enum {
    H264_DEC_OK = 0,
    H264_DEC_NO_PICTURE,
    H264_DEC_UNSUPPORTED,
    H264_DEC_BAD_DATA,
    H264_DEC_NO_MEMORY,
    H264_DEC_NO_FRAME,
} h264_dec_result_t;

typedef struct {
    void *(*sem_create)(void *ctx, uint32_t max, uint32_t initial);
    void (*sem_delete)(void *ctx, void *sem);
    void (*sem_take)(void *ctx, void *sem);
    void (*sem_give)(void *ctx, void *sem);
    bool (*spawn)(void *ctx, void (*entry)(void *arg), void *arg);
    void *ctx;
} h264_dec_threads_t;

typedef struct {
    void *(*alloc)(void *ctx, size_t bytes);
    void (*free)(void *ctx, void *ptr);
    void *ctx;
    uint8_t *work;
    size_t work_bytes;
    uint32_t max_mbs;
    uint32_t max_side;
    size_t frame_budget_bytes;
    uint8_t held_pictures;
    uint32_t (*clock)(void);
    const h264_dec_threads_t *threads;
} h264_dec_config_t;

typedef struct {
    uint16_t coded_width;
    uint16_t coded_height;
    uint16_t crop_left;
    uint16_t crop_top;
    uint16_t width;
    uint16_t height;
    bool full_range;
    uint8_t matrix_coefficients;
    uint8_t profile_idc;
    uint8_t level_idc;
    uint8_t max_ref_frames;
} h264_dec_stream_info_t;

typedef struct {
    const uint8_t *packed;
    size_t packed_bytes;
    h264_dec_stream_info_t info;
    int64_t tag;
    uint8_t id;
    bool reference;
    bool concealed;
} h264_dec_picture_t;

h264_dec_t *h264_dec_create(const h264_dec_config_t *config);
void h264_dec_destroy(h264_dec_t *dec);

h264_dec_result_t h264_dec_decode(h264_dec_t *dec, const uint8_t *data, size_t len,
                                  uint8_t nal_length_size, int64_t tag);
bool h264_dec_output(h264_dec_t *dec, h264_dec_picture_t *picture);
void h264_dec_drain(h264_dec_t *dec);

bool h264_dec_stream_info(const h264_dec_t *dec, h264_dec_stream_info_t *info);
bool h264_dec_probe(const uint8_t *data, size_t len, uint8_t nal_length_size,
                    h264_dec_stream_info_t *info, const char **error);
bool h264_dec_droppable(const uint8_t *data, size_t len, uint8_t nal_length_size);

void h264_dec_hold(h264_dec_t *dec, uint8_t id);
void h264_dec_release(h264_dec_t *dec, uint8_t id);
void h264_dec_flush(h264_dec_t *dec);

const char *h264_dec_error(const h264_dec_t *dec);

bool h264_dec_take_profile(h264_dec_t *dec, uint64_t out[H264_PROF_COUNT]);

#ifdef __cplusplus
}
#endif
