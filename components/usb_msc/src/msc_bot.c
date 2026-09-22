/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "msc_bot.h"

#include <string.h>

#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_memory_utils.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_helpers.h"

static const char *TAG = "msc_bot";

#define MSC_SUBCLASS_SCSI     0x06
#define MSC_PROTOCOL_BOT      0x50
#define MSC_REQUEST_RESET     0xFF
#define MSC_REQUEST_GET_LUN   0xFE

#define CBW_SIGNATURE         0x43425355
#define CSW_SIGNATURE         0x53425355
#define CBW_FLAG_DIR_IN       0x80
#define CBW_BYTES             31
#define CSW_BYTES             13

#define SCSI_TEST_UNIT_READY  0x00
#define SCSI_REQUEST_SENSE    0x03
#define SCSI_INQUIRY          0x12
#define SCSI_READ_CAPACITY10  0x25
#define SCSI_READ10           0x28
#define SCSI_WRITE10          0x2A

#define SENSE_NO_SENSE        0x00
#define SENSE_NOT_READY       0x02
#define SENSE_UNIT_ATTENTION  0x06

static const uint32_t kTransferTimeoutMs = 5000;
static const uint32_t kReadyTimeoutMs = 3000;
static const size_t kCommandBytes = 512;
static const size_t kBounceBytes = 4096;
static const uint32_t kMaxBlocksPerCommand = 65535;

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t data_length;
    uint8_t flags;
    uint8_t lun;
    uint8_t cb_length;
    uint8_t cb[16];
} msc_cbw_t;

typedef struct __attribute__((packed)) {
    uint32_t signature;
    uint32_t tag;
    uint32_t residue;
    uint8_t status;
} msc_csw_t;

struct msc_bot_device {
    usb_host_client_handle_t client;
    usb_device_handle_t handle;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t done;
    usb_transfer_t *cmd;
    usb_transfer_t *data;
    uint8_t *bounce;
    size_t bounce_bytes;
    uint32_t tag;
    uint32_t block_size;
    uint32_t block_count;
    size_t dma_alignment;
    size_t psram_alignment;
    uint8_t interface;
    uint8_t bulk_in;
    uint8_t bulk_out;
    uint16_t bulk_in_mps;
    uint16_t bulk_out_mps;
    bool claimed;
};

/* The host stack allocates transfer buffers itself, but hcd_dwc's
 * cache_sync_data_buffer() documents that class drivers may overwrite
 * data_buffer; swapping it in place is how a transfer lands straight in a
 * caller's buffer instead of being copied out of the stack's own. An IN
 * transfer is synced with ESP_CACHE_MSYNC_FLAG_DIR_M2C (no UNALIGNED), so a
 * borrowed buffer must be cache aligned in both address and size. */
static void transfer_set_buffer(usb_transfer_t *transfer, void *buffer, size_t bytes) {
    *(uint8_t **)&transfer->data_buffer = (uint8_t *)buffer;
    *(size_t *)&transfer->data_buffer_size = bytes;
}

static void transfer_done(usb_transfer_t *transfer) {
    msc_bot_device_t *device = (msc_bot_device_t *)transfer->context;
    xSemaphoreGive(device->done);
}

static esp_err_t status_to_err(usb_transfer_status_t status) {
    switch (status) {
    case USB_TRANSFER_STATUS_COMPLETED: return ESP_OK;
    case USB_TRANSFER_STATUS_STALL:     return ESP_ERR_INVALID_RESPONSE;
    case USB_TRANSFER_STATUS_NO_DEVICE: return ESP_ERR_NOT_FOUND;
    case USB_TRANSFER_STATUS_TIMED_OUT: return ESP_ERR_TIMEOUT;
    default:                            return ESP_FAIL;
    }
}

static esp_err_t await_transfer(msc_bot_device_t *device, usb_transfer_t *transfer) {
    if (xSemaphoreTake(device->done, pdMS_TO_TICKS(kTransferTimeoutMs)) != pdTRUE) {
        usb_host_endpoint_halt(device->handle, transfer->bEndpointAddress);
        usb_host_endpoint_flush(device->handle, transfer->bEndpointAddress);
        usb_host_endpoint_clear(device->handle, transfer->bEndpointAddress);
        xSemaphoreTake(device->done, portMAX_DELAY);
        return ESP_ERR_TIMEOUT;
    }
    return status_to_err(transfer->status);
}

static esp_err_t bulk_transfer(msc_bot_device_t *device, usb_transfer_t *transfer, uint8_t endpoint,
                               size_t bytes) {
    transfer->device_handle = device->handle;
    transfer->bEndpointAddress = endpoint;
    transfer->callback = transfer_done;
    transfer->context = device;
    transfer->timeout_ms = kTransferTimeoutMs;
    transfer->num_bytes = (int)bytes;
    const esp_err_t err = usb_host_transfer_submit(transfer);
    if (err != ESP_OK) return err;
    return await_transfer(device, transfer);
}

static esp_err_t control_transfer(msc_bot_device_t *device, uint8_t request_type, uint8_t request,
                                  uint16_t value, uint16_t index, uint16_t length) {
    usb_setup_packet_t *setup = (usb_setup_packet_t *)device->cmd->data_buffer;
    setup->bmRequestType = request_type;
    setup->bRequest = request;
    setup->wValue = value;
    setup->wIndex = index;
    setup->wLength = length;
    device->cmd->device_handle = device->handle;
    device->cmd->bEndpointAddress = 0;
    device->cmd->callback = transfer_done;
    device->cmd->context = device;
    device->cmd->timeout_ms = kTransferTimeoutMs;
    device->cmd->num_bytes = (int)(sizeof(usb_setup_packet_t) + length);
    const esp_err_t err = usb_host_transfer_submit_control(device->client, device->cmd);
    if (err != ESP_OK) return err;
    return await_transfer(device, device->cmd);
}

static esp_err_t clear_halt(msc_bot_device_t *device, uint8_t endpoint) {
    esp_err_t err = usb_host_endpoint_halt(device->handle, endpoint);
    if (err != ESP_OK) return err;
    err = usb_host_endpoint_flush(device->handle, endpoint);
    if (err != ESP_OK) return err;
    err = usb_host_endpoint_clear(device->handle, endpoint);
    if (err != ESP_OK) return err;
    return control_transfer(device, USB_BM_REQUEST_TYPE_DIR_OUT | USB_BM_REQUEST_TYPE_TYPE_STANDARD |
                            USB_BM_REQUEST_TYPE_RECIP_ENDPOINT, USB_B_REQUEST_CLEAR_FEATURE,
                            ENDPOINT_HALT, endpoint, 0);
}

static esp_err_t mass_storage_reset(msc_bot_device_t *device) {
    const esp_err_t err = control_transfer(device, USB_BM_REQUEST_TYPE_DIR_OUT |
                                           USB_BM_REQUEST_TYPE_TYPE_CLASS |
                                           USB_BM_REQUEST_TYPE_RECIP_INTERFACE,
                                           MSC_REQUEST_RESET, 0, device->interface, 0);
    clear_halt(device, device->bulk_in);
    clear_halt(device, device->bulk_out);
    return err;
}

/* Lending the caller's buffer to the transfer lets the USB DMA write it
   directly, which is what keeps a large read off the bounce buffer. A PSRAM
   destination then has to be kept coherent by hand (see data_stage), and that
   is only possible when the buffer starts and ends on a cache line, so the
   requirement is stricter there than for internal RAM. */
static bool borrowable(const msc_bot_device_t *device, const void *buffer, size_t bytes) {
    if (!buffer || !bytes) return false;
    const size_t alignment =
        esp_ptr_external_ram(buffer) ? device->psram_alignment : device->dma_alignment;
    return (uintptr_t)buffer % alignment == 0 && bytes % alignment == 0 &&
           bytes % device->bulk_in_mps == 0;
}

static esp_err_t data_stage(msc_bot_device_t *device, void *data, size_t bytes, bool in) {
    const uint8_t endpoint = in ? device->bulk_in : device->bulk_out;
    const bool borrowed = borrowable(device, data, bytes);
    /* The DMA reaches PSRAM behind the cache, so the caller's lines have to go
       out before the transfer -- otherwise a later write-back lands on top of
       what arrived -- and be dropped after a read, or the CPU keeps seeing what
       it cached before. Internal RAM needs neither. Getting this wrong does not
       fail: it returns a buffer that is right except for the lines the cache
       happened to hold, which reads as a file that is subtly different every
       time it is read. */
    const bool coherent = borrowed && esp_ptr_external_ram(data);
    if (borrowed) {
        transfer_set_buffer(device->data, data, bytes);
        if (coherent) {
            const esp_err_t sync = esp_cache_msync(data, bytes, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
            if (sync != ESP_OK) ESP_LOGE(TAG, "cache writeback: %s", esp_err_to_name(sync));
        }
    } else {
        transfer_set_buffer(device->data, device->bounce, device->bounce_bytes);
        if (!in) memcpy(device->bounce, data, bytes);
    }
    const size_t submit_bytes =
        in ? (size_t)usb_round_up_to_mps((int)bytes, device->bulk_in_mps) : bytes;
    const esp_err_t err = bulk_transfer(device, device->data, endpoint, submit_bytes);
    if (coherent && in) {
        const esp_err_t sync = esp_cache_msync(data, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);
        if (sync != ESP_OK) ESP_LOGE(TAG, "cache invalidate: %s", esp_err_to_name(sync));
    }
    if (err == ESP_OK && in && !borrowed) {
        memcpy(data, device->bounce, bytes);
    }
    transfer_set_buffer(device->data, device->bounce, device->bounce_bytes);
    return err;
}

static esp_err_t run_command(msc_bot_device_t *device, const uint8_t *cb, uint8_t cb_length,
                             void *data, size_t bytes, bool in) {
    msc_cbw_t *cbw = (msc_cbw_t *)device->cmd->data_buffer;
    memset(cbw, 0, sizeof(*cbw));
    cbw->signature = CBW_SIGNATURE;
    cbw->tag = ++device->tag;
    cbw->data_length = (uint32_t)bytes;
    cbw->flags = in ? CBW_FLAG_DIR_IN : 0;
    cbw->cb_length = cb_length;
    memcpy(cbw->cb, cb, cb_length);
    const uint32_t tag = cbw->tag;

    esp_err_t err = bulk_transfer(device, device->cmd, device->bulk_out, CBW_BYTES);
    if (err != ESP_OK) {
        if (err == ESP_ERR_INVALID_RESPONSE) mass_storage_reset(device);
        return err;
    }

    if (bytes) {
        err = data_stage(device, data, bytes, in);
        if (err == ESP_ERR_INVALID_RESPONSE) {
            clear_halt(device, in ? device->bulk_in : device->bulk_out);
        } else if (err != ESP_OK) {
            mass_storage_reset(device);
            return err;
        }
    }

    const size_t csw_bytes = (size_t)usb_round_up_to_mps(CSW_BYTES, device->bulk_in_mps);
    esp_err_t status = bulk_transfer(device, device->cmd, device->bulk_in, csw_bytes);
    if (status == ESP_ERR_INVALID_RESPONSE) {
        clear_halt(device, device->bulk_in);
        status = bulk_transfer(device, device->cmd, device->bulk_in, csw_bytes);
    }
    if (status != ESP_OK) {
        mass_storage_reset(device);
        return status;
    }

    const msc_csw_t *csw = (const msc_csw_t *)device->cmd->data_buffer;
    if (csw->signature != CSW_SIGNATURE || csw->tag != tag) {
        mass_storage_reset(device);
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (csw->status != 0 || csw->residue != 0) return ESP_FAIL;
    return err;
}

static esp_err_t scsi_request_sense(msc_bot_device_t *device, uint8_t *sense_key) {
    uint8_t response[18] = {0};
    const uint8_t cb[6] = { SCSI_REQUEST_SENSE, 0, 0, 0, sizeof(response), 0 };
    const esp_err_t err = run_command(device, cb, sizeof(cb), response, sizeof(response), true);
    if (sense_key) *sense_key = response[2] & 0x0F;
    return err;
}

static esp_err_t scsi_test_unit_ready(msc_bot_device_t *device) {
    const uint8_t cb[6] = { SCSI_TEST_UNIT_READY, 0, 0, 0, 0, 0 };
    return run_command(device, cb, sizeof(cb), NULL, 0, true);
}

static esp_err_t scsi_inquiry(msc_bot_device_t *device) {
    uint8_t response[36] = {0};
    const uint8_t cb[6] = { SCSI_INQUIRY, 0, 0, 0, sizeof(response), 0 };
    return run_command(device, cb, sizeof(cb), response, sizeof(response), true);
}

static esp_err_t scsi_read_capacity(msc_bot_device_t *device) {
    uint8_t response[8] = {0};
    const uint8_t cb[10] = { SCSI_READ_CAPACITY10, 0, 0, 0, 0, 0, 0, 0, 0, 0 };
    const esp_err_t err = run_command(device, cb, sizeof(cb), response, sizeof(response), true);
    if (err != ESP_OK) return err;
    const uint32_t last_block = ((uint32_t)response[0] << 24) | ((uint32_t)response[1] << 16) |
                                ((uint32_t)response[2] << 8) | response[3];
    device->block_size = ((uint32_t)response[4] << 24) | ((uint32_t)response[5] << 16) |
                         ((uint32_t)response[6] << 8) | response[7];
    device->block_count = last_block + 1;
    return ESP_OK;
}

static esp_err_t wait_until_ready(msc_bot_device_t *device) {
    const uint32_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(kReadyTimeoutMs);
    for (;;) {
        if (scsi_test_unit_ready(device) == ESP_OK) return ESP_OK;
        uint8_t sense_key = 0;
        const esp_err_t err = scsi_request_sense(device, &sense_key);
        if (err != ESP_OK) return err;
        if (sense_key != SENSE_NOT_READY && sense_key != SENSE_UNIT_ATTENTION &&
            sense_key != SENSE_NO_SENSE) {
            return ESP_ERR_INVALID_STATE;
        }
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static esp_err_t transfer_blocks(msc_bot_device_t *device, void *buffer, uint32_t block,
                                 uint32_t count, bool read) {
    uint8_t cb[10] = {0};
    cb[0] = read ? SCSI_READ10 : SCSI_WRITE10;
    cb[2] = (uint8_t)(block >> 24);
    cb[3] = (uint8_t)(block >> 16);
    cb[4] = (uint8_t)(block >> 8);
    cb[5] = (uint8_t)block;
    cb[7] = (uint8_t)(count >> 8);
    cb[8] = (uint8_t)count;
    return run_command(device, cb, sizeof(cb), buffer, (size_t)count * device->block_size, read);
}

static esp_err_t transfer_chunked(msc_bot_device_t *device, void *buffer, uint32_t block,
                                  uint32_t count, bool read) {
    if (!device || !buffer || !count) return ESP_ERR_INVALID_ARG;
    if (block > device->block_count || count > device->block_count - block) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint32_t bounce_blocks = (uint32_t)(device->bounce_bytes / device->block_size);
    uint8_t *cursor = (uint8_t *)buffer;
    esp_err_t err = ESP_OK;

    xSemaphoreTake(device->lock, portMAX_DELAY);
    while (count && err == ESP_OK) {
        uint32_t blocks = count > kMaxBlocksPerCommand ? kMaxBlocksPerCommand : count;
        if (!borrowable(device, cursor, (size_t)blocks * device->block_size) &&
            blocks > bounce_blocks) {
            blocks = bounce_blocks;
        }
        err = transfer_blocks(device, cursor, block, blocks, read);
        cursor += (size_t)blocks * device->block_size;
        block += blocks;
        count -= blocks;
    }
    xSemaphoreGive(device->lock);
    return err;
}

static const usb_intf_desc_t *find_interface(const usb_config_desc_t *config, int *offset) {
    const usb_standard_desc_t *desc = (const usb_standard_desc_t *)config;
    while ((desc = usb_parse_next_descriptor_of_type(desc, config->wTotalLength,
                                                     USB_W_VALUE_DT_INTERFACE, offset)) != NULL) {
        const usb_intf_desc_t *interface = (const usb_intf_desc_t *)desc;
        if (interface->bInterfaceClass == USB_CLASS_MASS_STORAGE &&
            interface->bInterfaceSubClass == MSC_SUBCLASS_SCSI &&
            interface->bInterfaceProtocol == MSC_PROTOCOL_BOT) {
            return interface;
        }
    }
    return NULL;
}

static esp_err_t find_endpoints(msc_bot_device_t *device, const usb_config_desc_t *config,
                                const usb_intf_desc_t *interface, int offset) {
    const usb_standard_desc_t *desc = (const usb_standard_desc_t *)interface;
    for (int i = 0; i < interface->bNumEndpoints; i++) {
        desc = usb_parse_next_descriptor_of_type(desc, config->wTotalLength,
                                                 USB_B_DESCRIPTOR_TYPE_ENDPOINT, &offset);
        if (!desc) break;
        const usb_ep_desc_t *endpoint = (const usb_ep_desc_t *)desc;
        if (USB_EP_DESC_GET_XFERTYPE(endpoint) != USB_TRANSFER_TYPE_BULK) continue;
        if (USB_EP_DESC_GET_EP_DIR(endpoint)) {
            device->bulk_in = endpoint->bEndpointAddress;
            device->bulk_in_mps = USB_EP_DESC_GET_MPS(endpoint);
        } else {
            device->bulk_out = endpoint->bEndpointAddress;
            device->bulk_out_mps = USB_EP_DESC_GET_MPS(endpoint);
        }
    }
    return device->bulk_in && device->bulk_out ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
}

static size_t cache_alignment(uint32_t caps) {
    size_t alignment = 0;
    esp_cache_get_alignment(caps, &alignment);
    return alignment ? alignment : 4;
}

esp_err_t msc_bot_open(usb_host_client_handle_t client, uint8_t address,
                       msc_bot_device_t **out_device) {
    if (!client || !out_device) return ESP_ERR_INVALID_ARG;

    msc_bot_device_t *device = heap_caps_calloc(1, sizeof(*device), MALLOC_CAP_DEFAULT);
    if (!device) return ESP_ERR_NO_MEM;
    device->client = client;
    device->dma_alignment = cache_alignment(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    device->psram_alignment = cache_alignment(MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
    device->bounce_bytes = kBounceBytes;
    device->lock = xSemaphoreCreateMutex();
    device->done = xSemaphoreCreateBinary();

    esp_err_t err = (device->lock && device->done) ? ESP_OK : ESP_ERR_NO_MEM;
    if (err == ESP_OK) err = usb_host_device_open(client, address, &device->handle);

    const usb_config_desc_t *config = NULL;
    if (err == ESP_OK) err = usb_host_get_active_config_descriptor(device->handle, &config);

    int offset = 0;
    const usb_intf_desc_t *interface = NULL;
    if (err == ESP_OK) {
        interface = find_interface(config, &offset);
        err = interface ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK) {
        device->interface = interface->bInterfaceNumber;
        err = find_endpoints(device, config, interface, offset);
    }
    if (err == ESP_OK) err = usb_host_transfer_alloc(kCommandBytes, 0, &device->cmd);
    if (err == ESP_OK) err = usb_host_transfer_alloc(device->bounce_bytes, 0, &device->data);
    if (err == ESP_OK) {
        device->bounce = device->data->data_buffer;
        err = usb_host_interface_claim(client, device->handle, device->interface, 0);
        device->claimed = err == ESP_OK;
    }
    if (err == ESP_OK) err = scsi_inquiry(device);
    if (err == ESP_OK) err = wait_until_ready(device);
    if (err == ESP_OK) err = scsi_read_capacity(device);
    if (err == ESP_OK && (device->block_size < 512 || device->block_size > kBounceBytes ||
                          (device->block_size & (device->block_size - 1)) != 0)) {
        ESP_LOGE(TAG, "unsupported block size %u", (unsigned)device->block_size);
        err = ESP_ERR_NOT_SUPPORTED;
    }
    if (err != ESP_OK) {
        msc_bot_close(device);
        return err;
    }

    ESP_LOGI(TAG, "drive at %u: %u blocks of %u bytes, bulk in %02x/%u out %02x/%u",
             address, (unsigned)device->block_count, (unsigned)device->block_size,
             device->bulk_in, device->bulk_in_mps, device->bulk_out, device->bulk_out_mps);
    *out_device = device;
    return ESP_OK;
}

esp_err_t msc_bot_close(msc_bot_device_t *device) {
    if (!device) return ESP_ERR_INVALID_ARG;
    if (device->claimed) usb_host_interface_release(device->client, device->handle,
                                                    device->interface);
    if (device->data) {
        transfer_set_buffer(device->data, device->bounce, device->bounce_bytes);
        usb_host_transfer_free(device->data);
    }
    if (device->cmd) usb_host_transfer_free(device->cmd);
    if (device->handle) usb_host_device_close(device->client, device->handle);
    if (device->lock) vSemaphoreDelete(device->lock);
    if (device->done) vSemaphoreDelete(device->done);
    heap_caps_free(device);
    return ESP_OK;
}

usb_device_handle_t msc_bot_usb_handle(const msc_bot_device_t *device) {
    return device ? device->handle : NULL;
}

uint32_t msc_bot_block_size(const msc_bot_device_t *device) {
    return device ? device->block_size : 0;
}

uint32_t msc_bot_block_count(const msc_bot_device_t *device) {
    return device ? device->block_count : 0;
}

esp_err_t msc_bot_read(msc_bot_device_t *device, void *dst, uint32_t block, uint32_t count) {
    return transfer_chunked(device, dst, block, count, true);
}

esp_err_t msc_bot_write(msc_bot_device_t *device, const void *src, uint32_t block, uint32_t count) {
    return transfer_chunked(device, (void *)src, block, count, false);
}
