/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "usb_msc.h"

#include "bsp.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/msc_host_vfs.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_msc";

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_events;
static usb_msc_event_cb_t s_cb;
static void *s_cb_arg;

static msc_host_device_handle_t s_device;
static msc_host_vfs_handle_t s_vfs;
static bool s_gone;
static uint8_t s_pending_addr;

static void notify(usb_msc_event_t event) {
    if (s_cb) s_cb(event, s_cb_arg);
}

static void host_lib_task(void *arg) {
    (void)arg;
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void msc_event_cb(const msc_host_event_t *event, void *arg) {
    (void)arg;
    xQueueSend(s_events, event, 0);
}

static void handle_connected(uint8_t addr) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = s_device != NULL;
    const bool replaced = busy && s_gone;
    if (replaced) s_pending_addr = addr;
    xSemaphoreGive(s_lock);
    if (replaced) {
        notify(USB_MSC_EVENT_CONNECTED);
        return;
    }
    if (busy) {
        ESP_LOGW(TAG, "ignoring second drive at addr %u", addr);
        return;
    }
    msc_host_device_handle_t device = NULL;
    esp_err_t err = msc_host_install_device(addr, &device);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "msc_host_install_device: %s", esp_err_to_name(err));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_device = device;
    xSemaphoreGive(s_lock);
    notify(USB_MSC_EVENT_CONNECTED);
}

static void handle_disconnected(msc_host_device_handle_t device) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (device != s_device) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (s_vfs) {
        s_gone = true;
    } else {
        msc_host_uninstall_device(s_device);
        s_device = NULL;
    }
    xSemaphoreGive(s_lock);
    notify(USB_MSC_EVENT_DISCONNECTED);
}

static void worker_task(void *arg) {
    (void)arg;
    msc_host_event_t event;
    while (true) {
        xQueueReceive(s_events, &event, portMAX_DELAY);
        if (event.event == MSC_DEVICE_CONNECTED) {
            handle_connected(event.device.address);
        } else if (event.event == MSC_DEVICE_DISCONNECTED) {
            handle_disconnected(event.device.handle);
        }
    }
}

esp_err_t usb_msc_init(usb_msc_event_cb_t cb, void *arg) {
    if (s_lock) return ESP_ERR_INVALID_STATE;
    s_cb = cb;
    s_cb_arg = arg;
    s_lock = xSemaphoreCreateMutex();
    s_events = xQueueCreate(4, sizeof(msc_host_event_t));
    if (!s_lock || !s_events) return ESP_ERR_NO_MEM;

    esp_err_t err = bsp_power_set_switch(BSP_POWER_SWITCH_USB5V, true);
    if (err != ESP_OK && err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "USB 5V: %s", esp_err_to_name(err));
    }

    const usb_host_config_t host_config = {
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    err = usb_host_install(&host_config);
    if (err != ESP_OK) return err;
    if (xTaskCreate(host_lib_task, "usb_host", 3072, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker_task, "usb_msc", 3072, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;

    const msc_host_driver_config_t msc_config = {
        .create_backround_task = true,
        .task_priority = 5,
        .stack_size = 4096,
        .core_id = tskNO_AFFINITY,
        .callback = msc_event_cb,
    };
    return msc_host_install(&msc_config);
}

bool usb_msc_is_connected(void) {
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool connected = (s_device && !s_gone) || s_pending_addr;
    xSemaphoreGive(s_lock);
    return connected;
}

static void release_locked(void) {
    if (s_vfs) {
        msc_host_vfs_unregister(s_vfs);
        s_vfs = NULL;
    }
    if (s_gone) {
        msc_host_uninstall_device(s_device);
        s_device = NULL;
        s_gone = false;
    }
}

static esp_err_t mount_locked(const char *mount_point, uint8_t max_files) {
    if (s_vfs && !s_gone) return ESP_ERR_INVALID_STATE;
    release_locked();
    if (!s_device && s_pending_addr) {
        const uint8_t addr = s_pending_addr;
        s_pending_addr = 0;
        esp_err_t err = msc_host_install_device(addr, &s_device);
        if (err != ESP_OK) {
            s_device = NULL;
            return err;
        }
    }
    if (!s_device) return ESP_ERR_NOT_FOUND;
    const esp_vfs_fat_mount_config_t mount_config = {
        .max_files = max_files > 0 ? max_files : 5,
    };
    esp_err_t err = msc_host_vfs_register(s_device, mount_point, &mount_config, &s_vfs);
    if (err != ESP_OK) s_vfs = NULL;
    return err;
}

esp_err_t usb_msc_mount(const char *mount_point, uint8_t max_files) {
    if (!mount_point) return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const esp_err_t err = mount_locked(mount_point, max_files);
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t usb_msc_unmount(void) {
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool mounted = s_vfs != NULL;
    if (mounted) release_locked();
    xSemaphoreGive(s_lock);
    return mounted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool usb_msc_is_mounted(void) {
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool mounted = s_vfs && !s_gone;
    xSemaphoreGive(s_lock);
    return mounted;
}
