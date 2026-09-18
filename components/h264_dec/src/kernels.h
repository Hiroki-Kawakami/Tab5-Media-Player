/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stddef.h>
#include <stdint.h>
#include "vdec_kernels.h"

void h264_k_tap6_u8(const uint8_t *src, ptrdiff_t step, uint8_t *dst, int rows,
                    ptrdiff_t src_stride, ptrdiff_t dst_stride);
void h264_k_tap6_s16(const uint8_t *src, int16_t *dst, int rows, ptrdiff_t src_stride);
void h264_k_tap6v_s16(const int16_t *mid, uint8_t *dst, int rows, ptrdiff_t dst_stride);
void h264_k_weight_u8(uint8_t *dst, ptrdiff_t stride, int rows, int w, int round, int shift,
                      int offset);
void h264_k_weight_bi_u8(const uint8_t *a, const uint8_t *b, uint8_t *dst, int rows,
                         ptrdiff_t src_stride, ptrdiff_t dst_stride, int w1);

