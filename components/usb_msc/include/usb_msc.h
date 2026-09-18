/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    USB_MSC_EVENT_CONNECTED,
    USB_MSC_EVENT_DISCONNECTED,
} usb_msc_event_t;

typedef void (*usb_msc_event_cb_t)(usb_msc_event_t event, void *arg);

esp_err_t usb_msc_init(usb_msc_event_cb_t cb, void *arg);
bool      usb_msc_is_connected(void);
esp_err_t usb_msc_mount(const char *mount_point, uint8_t max_files);
esp_err_t usb_msc_unmount(void);
bool      usb_msc_is_mounted(void);

#ifdef __cplusplus
}
#endif
