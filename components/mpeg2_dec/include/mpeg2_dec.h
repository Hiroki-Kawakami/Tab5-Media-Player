/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "vdec_threads.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MPEG2_DEC_WORK_ALIGNMENT 16

typedef enum {
    MPEG2_PROF_VLC,
    MPEG2_PROF_IDCT,
    MPEG2_PROF_MC,
    MPEG2_PROF_FETCH,
    MPEG2_PROF_FLUSH,
    MPEG2_PROF_WAIT,
    MPEG2_PROF_TOTAL,
    MPEG2_PROF_COUNT,
} mpeg2_prof_stage_t;

typedef struct mpeg2_dec mpeg2_dec_t;

typedef enum {
    MPEG2_DEC_OK = 0,
    MPEG2_DEC_NO_PICTURE,
    MPEG2_DEC_UNSUPPORTED,
    MPEG2_DEC_BAD_DATA,
    MPEG2_DEC_NO_MEMORY,
    MPEG2_DEC_NO_FRAME,
} mpeg2_dec_result_t;

typedef enum {
    MPEG2_PICTURE_I = 1,
    MPEG2_PICTURE_P = 2,
    MPEG2_PICTURE_B = 3,
} mpeg2_picture_type_t;

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
    const vdec_threads_t *threads;
} mpeg2_dec_config_t;

typedef struct {
    uint16_t coded_width;
    uint16_t coded_height;
    uint16_t width;
    uint16_t height;
    uint8_t matrix_coefficients;
    uint8_t profile_and_level;
} mpeg2_dec_stream_info_t;

typedef struct {
    const uint8_t *packed;
    size_t packed_bytes;
    mpeg2_dec_stream_info_t info;
    int64_t tag;
    uint8_t id;
    uint8_t type;
    bool concealed;
} mpeg2_dec_picture_t;

typedef struct {
    bool gop;
    bool closed_gop;
    uint8_t type;
    uint16_t temporal_reference;
} mpeg2_dec_header_t;

mpeg2_dec_t *mpeg2_dec_create(const mpeg2_dec_config_t *config);
void mpeg2_dec_destroy(mpeg2_dec_t *dec);

mpeg2_dec_result_t mpeg2_dec_decode(mpeg2_dec_t *dec, const uint8_t *data, size_t len, int64_t tag);
bool mpeg2_dec_output(mpeg2_dec_t *dec, mpeg2_dec_picture_t *picture);
void mpeg2_dec_drain(mpeg2_dec_t *dec);

bool mpeg2_dec_stream_info(const mpeg2_dec_t *dec, mpeg2_dec_stream_info_t *info);
bool mpeg2_dec_probe(const uint8_t *data, size_t len, mpeg2_dec_stream_info_t *info,
                     const char **error);
bool mpeg2_dec_header(const uint8_t *data, size_t len, mpeg2_dec_header_t *header);
bool mpeg2_dec_droppable(const uint8_t *data, size_t len);

void mpeg2_dec_hold(mpeg2_dec_t *dec, uint8_t id);
void mpeg2_dec_release(mpeg2_dec_t *dec, uint8_t id);
void mpeg2_dec_flush(mpeg2_dec_t *dec);

const char *mpeg2_dec_error(const mpeg2_dec_t *dec);

bool mpeg2_dec_take_profile(mpeg2_dec_t *dec, uint64_t out[MPEG2_PROF_COUNT]);
int mpeg2_dec_idct_selftest(uint32_t seed, int iterations, int max_level);

#ifdef __cplusplus
}
#endif
