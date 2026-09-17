/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stddef.h>
#include <stdint.h>

void h264_k_tap6_u8(const uint8_t *src, ptrdiff_t step, uint8_t *dst, int rows,
                    ptrdiff_t src_stride, ptrdiff_t dst_stride);
void h264_k_tap6_s16(const uint8_t *src, int16_t *dst, int rows, ptrdiff_t src_stride);
void h264_k_avg_u8(const uint8_t *a, const uint8_t *b, uint8_t *dst, int rows, ptrdiff_t a_stride,
                   ptrdiff_t b_stride, ptrdiff_t dst_stride);
void h264_k_bilinear(const uint8_t *src, ptrdiff_t src_stride, uint8_t *dst, ptrdiff_t dst_stride,
                     int rows, int fx, int fy);
void h264_k_pack(const uint8_t *c, const uint8_t *y, uint8_t *dst, int blocks);
void h264_k_unpack(const uint8_t *src, uint8_t *c, uint8_t *y, int blocks);
void h264_k_unpack_rows(const uint8_t *src, ptrdiff_t src_stride, uint8_t *c, ptrdiff_t c_stride,
                        uint8_t *y, ptrdiff_t y_stride, int rows);
void h264_k_prepare(void);
