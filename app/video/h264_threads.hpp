/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "h264_dec.h"

const h264_dec_threads_t *h264_threads();

BaseType_t h264_create_task(TaskFunction_t entry, const char *name, uint32_t stack_bytes,
                            void *arg, UBaseType_t priority, BaseType_t core,
                            TaskHandle_t *handle);
