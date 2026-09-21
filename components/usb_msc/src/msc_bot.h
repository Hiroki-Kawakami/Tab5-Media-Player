/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"
#include "usb/usb_host.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct msc_bot_device msc_bot_device_t;

esp_err_t msc_bot_open(usb_host_client_handle_t client, uint8_t address,
                       msc_bot_device_t **out_device);
esp_err_t msc_bot_close(msc_bot_device_t *device);

usb_device_handle_t msc_bot_usb_handle(const msc_bot_device_t *device);
uint32_t msc_bot_block_size(const msc_bot_device_t *device);
uint32_t msc_bot_block_count(const msc_bot_device_t *device);

esp_err_t msc_bot_read(msc_bot_device_t *device, void *dst, uint32_t block, uint32_t count);
esp_err_t msc_bot_write(msc_bot_device_t *device, const void *src, uint32_t block, uint32_t count);

#ifdef __cplusplus
}
#endif
