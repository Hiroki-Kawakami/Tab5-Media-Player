/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "usb_msc.h"

#include <string.h>

#include "bsp.h"
#include "diskio_impl.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "esp_vfs.h"
#include "esp_vfs_fat.h"
#include "ff.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "msc_bot.h"
#include "usb/usb_host.h"

static const char *TAG = "usb_msc";

typedef enum {
    CLIENT_EVENT_CONNECTED,
    CLIENT_EVENT_GONE,
} client_event_type_t;

typedef struct {
    client_event_type_t type;
    uint8_t address;
    usb_device_handle_t handle;
} client_event_t;

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_events;
static usb_msc_event_cb_t s_cb;
static void *s_cb_arg;

static usb_host_client_handle_t s_client;
static msc_bot_device_t *s_device;
static bool s_gone;
static uint8_t s_pending_addr;

static char s_base_path[ESP_VFS_PATH_MAX + 1];
static char s_drive[3];
static BYTE s_pdrv = FF_DRV_NOT_USED;

static void notify(usb_msc_event_t event) {
    if (s_cb) s_cb(event, s_cb_arg);
}

static DSTATUS diskio_initialize(BYTE pdrv) {
    (void)pdrv;
    return s_device ? 0 : STA_NOINIT;
}

static DSTATUS diskio_status(BYTE pdrv) {
    (void)pdrv;
    return s_device ? 0 : STA_NOINIT;
}

static DRESULT diskio_read(BYTE pdrv, BYTE *buffer, LBA_t sector, UINT count) {
    (void)pdrv;
    if (!s_device) return RES_NOTRDY;
    return msc_bot_read(s_device, buffer, (uint32_t)sector, count) == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT diskio_write(BYTE pdrv, const BYTE *buffer, LBA_t sector, UINT count) {
    (void)pdrv;
    if (!s_device) return RES_NOTRDY;
    return msc_bot_write(s_device, buffer, (uint32_t)sector, count) == ESP_OK ? RES_OK : RES_ERROR;
}

static DRESULT diskio_ioctl(BYTE pdrv, BYTE cmd, void *buffer) {
    (void)pdrv;
    if (!s_device) return RES_NOTRDY;
    switch (cmd) {
    case CTRL_SYNC:
        return RES_OK;
    case GET_SECTOR_COUNT:
        *(LBA_t *)buffer = msc_bot_block_count(s_device);
        return RES_OK;
    case GET_SECTOR_SIZE:
        *(WORD *)buffer = (WORD)msc_bot_block_size(s_device);
        return RES_OK;
    case GET_BLOCK_SIZE:
        *(DWORD *)buffer = 1;
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

static const ff_diskio_impl_t kDiskio = {
    .init = diskio_initialize,
    .status = diskio_status,
    .read = diskio_read,
    .write = diskio_write,
    .ioctl = diskio_ioctl,
};

static void host_lib_task(void *arg) {
    (void)arg;
    while (true) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void client_event_cb(const usb_host_client_event_msg_t *message, void *arg) {
    (void)arg;
    client_event_t event = {0};
    if (message->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        event.type = CLIENT_EVENT_CONNECTED;
        event.address = message->new_dev.address;
    } else if (message->event == USB_HOST_CLIENT_EVENT_DEV_GONE) {
        event.type = CLIENT_EVENT_GONE;
        event.handle = message->dev_gone.dev_hdl;
    } else {
        return;
    }
    xQueueSend(s_events, &event, 0);
}

static void client_task(void *arg) {
    (void)arg;
    while (true) {
        usb_host_client_handle_events(s_client, portMAX_DELAY);
    }
}

static void handle_connected(uint8_t address) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool busy = s_device != NULL;
    const bool replaced = busy && s_gone;
    if (replaced) s_pending_addr = address;
    xSemaphoreGive(s_lock);
    if (replaced) {
        notify(USB_MSC_EVENT_CONNECTED);
        return;
    }
    if (busy) {
        ESP_LOGW(TAG, "ignoring second drive at addr %u", address);
        return;
    }
    msc_bot_device_t *device = NULL;
    const esp_err_t err = msc_bot_open(s_client, address, &device);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NOT_SUPPORTED) ESP_LOGE(TAG, "open: %s", esp_err_to_name(err));
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_device = device;
    xSemaphoreGive(s_lock);
    notify(USB_MSC_EVENT_CONNECTED);
}

static void handle_disconnected(usb_device_handle_t handle) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_device || msc_bot_usb_handle(s_device) != handle) {
        xSemaphoreGive(s_lock);
        return;
    }
    if (s_pdrv != FF_DRV_NOT_USED) {
        s_gone = true;
    } else {
        msc_bot_close(s_device);
        s_device = NULL;
    }
    xSemaphoreGive(s_lock);
    notify(USB_MSC_EVENT_DISCONNECTED);
}

static void worker_task(void *arg) {
    (void)arg;
    client_event_t event;
    while (true) {
        xQueueReceive(s_events, &event, portMAX_DELAY);
        if (event.type == CLIENT_EVENT_CONNECTED) {
            handle_connected(event.address);
        } else {
            handle_disconnected(event.handle);
        }
    }
}

esp_err_t usb_msc_init(usb_msc_event_cb_t cb, void *arg) {
    if (s_lock) return ESP_ERR_INVALID_STATE;
    s_cb = cb;
    s_cb_arg = arg;
    s_lock = xSemaphoreCreateMutex();
    s_events = xQueueCreate(4, sizeof(client_event_t));
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

    const usb_host_client_config_t client_config = {
        .is_synchronous = false,
        .max_num_event_msg = 10,
        .async = {
            .client_event_callback = client_event_cb,
            .callback_arg = NULL,
        },
    };
    err = usb_host_client_register(&client_config, &s_client);
    if (err != ESP_OK) return err;

    if (xTaskCreate(host_lib_task, "usb_host", 3072, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(client_task, "usb_client", 4096, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    if (xTaskCreate(worker_task, "usb_msc", 4096, NULL, 5, NULL) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool usb_msc_is_connected(void) {
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool connected = (s_device && !s_gone) || s_pending_addr;
    xSemaphoreGive(s_lock);
    return connected;
}

static void unmount_locked(void) {
    if (s_pdrv != FF_DRV_NOT_USED) {
        f_mount(NULL, s_drive, 0);
        ff_diskio_unregister(s_pdrv);
        esp_vfs_fat_unregister_path(s_base_path);
        s_pdrv = FF_DRV_NOT_USED;
        s_base_path[0] = '\0';
    }
    if (s_gone) {
        msc_bot_close(s_device);
        s_device = NULL;
        s_gone = false;
    }
}

static esp_err_t mount_locked(const char *mount_point, uint8_t max_files) {
    if (s_pdrv != FF_DRV_NOT_USED && !s_gone) return ESP_ERR_INVALID_STATE;
    unmount_locked();
    if (!s_device && s_pending_addr) {
        const uint8_t address = s_pending_addr;
        s_pending_addr = 0;
        const esp_err_t err = msc_bot_open(s_client, address, &s_device);
        if (err != ESP_OK) {
            s_device = NULL;
            return err;
        }
    }
    if (!s_device) return ESP_ERR_NOT_FOUND;
    if (strlen(mount_point) > ESP_VFS_PATH_MAX) return ESP_ERR_INVALID_ARG;

    BYTE pdrv = FF_DRV_NOT_USED;
    esp_err_t err = ff_diskio_get_drive(&pdrv);
    if (err != ESP_OK) return err;
    ff_diskio_register(pdrv, &kDiskio);
    s_drive[0] = (char)('0' + pdrv);
    s_drive[1] = ':';
    s_drive[2] = '\0';

    FATFS *fs = NULL;
    const esp_vfs_fat_conf_t conf = {
        .base_path = mount_point,
        .fat_drive = s_drive,
        .max_files = max_files > 0 ? max_files : 5,
    };
    err = esp_vfs_fat_register(&conf, &fs);
    if (err != ESP_OK) {
        ff_diskio_unregister(pdrv);
        return err;
    }
    if (f_mount(fs, s_drive, 1) != FR_OK) {
        esp_vfs_fat_unregister_path(mount_point);
        ff_diskio_unregister(pdrv);
        return ESP_FAIL;
    }
    strlcpy(s_base_path, mount_point, sizeof(s_base_path));
    s_pdrv = pdrv;
    return ESP_OK;
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
    const bool mounted = s_pdrv != FF_DRV_NOT_USED;
    if (mounted) unmount_locked();
    xSemaphoreGive(s_lock);
    return mounted ? ESP_OK : ESP_ERR_INVALID_STATE;
}

bool usb_msc_is_mounted(void) {
    if (!s_lock) return false;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool mounted = s_pdrv != FF_DRV_NOT_USED && !s_gone;
    xSemaphoreGive(s_lock);
    return mounted;
}
