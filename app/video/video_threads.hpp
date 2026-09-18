/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "vdec_threads.h"

vdec_threads_t video_threads(const char *worker_name);

BaseType_t video_create_task(TaskFunction_t entry, const char *name, uint32_t stack_bytes,
                             void *arg, UBaseType_t priority, BaseType_t core,
                             TaskHandle_t *handle);
