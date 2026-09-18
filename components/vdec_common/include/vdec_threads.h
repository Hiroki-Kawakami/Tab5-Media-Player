/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    void *(*sem_create)(void *ctx, uint32_t max, uint32_t initial);
    void (*sem_delete)(void *ctx, void *sem);
    void (*sem_take)(void *ctx, void *sem);
    void (*sem_give)(void *ctx, void *sem);
    bool (*spawn)(void *ctx, void (*entry)(void *arg), void *arg);
    void *ctx;
} vdec_threads_t;
