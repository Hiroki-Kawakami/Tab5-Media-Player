/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "buffered_reader.h"

#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "avi_reader";

#define BR_CHUNK_BYTES (64 * 1024)
#define BR_CHUNK_COUNT 16
#define BR_IDLE_WAIT_MS 20
#define BR_STOP_TIMEOUT_MS 2000

struct buffered_reader {
    int fd;
    off_t size;
    off_t cursor;
    off_t base;
    int head;
    int filled;
    bool readahead;
    bool running;
    volatile bool stop;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t io_lock;
    SemaphoreHandle_t wake;
    SemaphoreHandle_t done;
    uint8_t *chunk[BR_CHUNK_COUNT];
};

static off_t align_down(off_t value, off_t alignment) {
    return value - (value % alignment);
}

static size_t read_at(buffered_reader_t *reader, off_t offset, void *buffer, size_t size) {
    xSemaphoreTake(reader->io_lock, portMAX_DELAY);
    size_t got = 0;
    if (lseek(reader->fd, offset, SEEK_SET) == offset) {
        while (got < size) {
            const ssize_t n = read(reader->fd, (uint8_t *)buffer + got, size - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
    }
    xSemaphoreGive(reader->io_lock);
    return got;
}

static void drop_consumed(buffered_reader_t *reader) {
    if (reader->cursor < reader->base) {
        reader->base = align_down(reader->cursor, BR_CHUNK_BYTES);
        reader->head = 0;
        reader->filled = 0;
        return;
    }
    while (reader->filled > 0 && reader->base + BR_CHUNK_BYTES <= reader->cursor) {
        reader->base += BR_CHUNK_BYTES;
        reader->head = (reader->head + 1) % BR_CHUNK_COUNT;
        reader->filled--;
    }
    if (reader->filled == 0) reader->base = align_down(reader->cursor, BR_CHUNK_BYTES);
}

static void readahead_task(void *arg) {
    buffered_reader_t *reader = (buffered_reader_t *)arg;

    while (!reader->stop) {
        off_t offset = 0;
        int slot = -1;

        xSemaphoreTake(reader->lock, portMAX_DELAY);
        if (reader->readahead) {
            drop_consumed(reader);
            offset = reader->base + (off_t)reader->filled * BR_CHUNK_BYTES;
            if (reader->filled < BR_CHUNK_COUNT && offset < reader->size) {
                slot = (reader->head + reader->filled) % BR_CHUNK_COUNT;
            }
        }
        xSemaphoreGive(reader->lock);

        if (slot < 0) {
            xSemaphoreTake(reader->wake, pdMS_TO_TICKS(BR_IDLE_WAIT_MS));
            continue;
        }

        size_t want = BR_CHUNK_BYTES;
        if (offset + (off_t)want > reader->size) want = (size_t)(reader->size - offset);
        const size_t got = read_at(reader, offset, reader->chunk[slot], want);

        xSemaphoreTake(reader->lock, portMAX_DELAY);
        if (got == want && reader->readahead && reader->filled < BR_CHUNK_COUNT &&
            reader->base + (off_t)reader->filled * BR_CHUNK_BYTES == offset) {
            reader->filled++;
        }
        xSemaphoreGive(reader->lock);
    }

    xSemaphoreGive(reader->done);
    vTaskDelete(NULL);
}

buffered_reader_t *br_open(const char *path) {
    const int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;

    buffered_reader_t *reader = heap_caps_calloc(1, sizeof(*reader), MALLOC_CAP_DEFAULT);
    if (!reader) {
        close(fd);
        return NULL;
    }
    reader->fd = fd;

    struct stat info;
    if (fstat(fd, &info) != 0) {
        close(fd);
        heap_caps_free(reader);
        return NULL;
    }
    reader->size = info.st_size;

    reader->lock = xSemaphoreCreateMutex();
    reader->io_lock = xSemaphoreCreateMutex();
    reader->wake = xSemaphoreCreateBinary();
    reader->done = xSemaphoreCreateBinary();
    if (!reader->lock || !reader->io_lock || !reader->wake || !reader->done) {
        br_close(reader);
        return NULL;
    }

    for (int i = 0; i < BR_CHUNK_COUNT; i++) {
        reader->chunk[i] = heap_caps_aligned_alloc(64, BR_CHUNK_BYTES,
                                                   MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
        if (!reader->chunk[i]) {
            reader->chunk[i] = heap_caps_aligned_alloc(64, BR_CHUNK_BYTES, MALLOC_CAP_DEFAULT);
        }
        if (!reader->chunk[i]) {
            ESP_LOGE(TAG, "out of memory for the read-ahead ring");
            br_close(reader);
            return NULL;
        }
    }

    if (xTaskCreatePinnedToCore(readahead_task, "avi_reader", 3072, reader, 3, NULL, 0) != pdPASS) {
        br_close(reader);
        return NULL;
    }
    reader->running = true;
    return reader;
}

void br_close(buffered_reader_t *reader) {
    if (!reader) return;

    if (reader->running) {
        reader->stop = true;
        xSemaphoreGive(reader->wake);
        if (xSemaphoreTake(reader->done, pdMS_TO_TICKS(BR_STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "read-ahead did not stop; leaking the reader");
            return;
        }
        reader->running = false;
    }

    for (int i = 0; i < BR_CHUNK_COUNT; i++) heap_caps_free(reader->chunk[i]);
    if (reader->lock) vSemaphoreDelete(reader->lock);
    if (reader->io_lock) vSemaphoreDelete(reader->io_lock);
    if (reader->wake) vSemaphoreDelete(reader->wake);
    if (reader->done) vSemaphoreDelete(reader->done);
    close(reader->fd);
    heap_caps_free(reader);
}

off_t br_size(const buffered_reader_t *reader) { return reader->size; }
off_t br_tell(const buffered_reader_t *reader) { return reader->cursor; }

size_t br_read(buffered_reader_t *reader, void *buffer, size_t size) {
    if (reader->cursor >= reader->size) return 0;
    if (reader->cursor + (off_t)size > reader->size) size = (size_t)(reader->size - reader->cursor);

    xSemaphoreTake(reader->lock, portMAX_DELAY);
    const off_t base = reader->base;
    const off_t end = base + (off_t)reader->filled * BR_CHUNK_BYTES;
    const bool cached = reader->readahead && reader->filled > 0 &&
                        reader->cursor >= base && reader->cursor + (off_t)size <= end;

    if (!cached) {
        xSemaphoreGive(reader->lock);
        const size_t got = read_at(reader, reader->cursor, buffer, size);
        xSemaphoreTake(reader->lock, portMAX_DELAY);
        reader->cursor += got;
        xSemaphoreGive(reader->lock);
        xSemaphoreGive(reader->wake);
        return got;
    }

    uint8_t *out = (uint8_t *)buffer;
    size_t left = size;
    while (left > 0) {
        const int index = (int)((reader->cursor - base) / BR_CHUNK_BYTES);
        const size_t offset = (size_t)((reader->cursor - base) % BR_CHUNK_BYTES);
        size_t take = BR_CHUNK_BYTES - offset;
        if (take > left) take = left;
        memcpy(out, reader->chunk[(reader->head + index) % BR_CHUNK_COUNT] + offset, take);
        out += take;
        left -= take;
        reader->cursor += take;
    }
    xSemaphoreGive(reader->lock);
    xSemaphoreGive(reader->wake);
    return size;
}

off_t br_seek(buffered_reader_t *reader, off_t offset) {
    if (offset < 0) offset = 0;
    if (offset > reader->size) offset = reader->size;
    xSemaphoreTake(reader->lock, portMAX_DELAY);
    reader->cursor = offset;
    drop_consumed(reader);
    xSemaphoreGive(reader->lock);
    xSemaphoreGive(reader->wake);
    return offset;
}

void br_skip(buffered_reader_t *reader, off_t delta) {
    br_seek(reader, reader->cursor + delta);
}

void br_set_readahead(buffered_reader_t *reader, bool enabled) {
    xSemaphoreTake(reader->lock, portMAX_DELAY);
    reader->readahead = enabled;
    if (!enabled) {
        reader->filled = 0;
        reader->head = 0;
        reader->base = align_down(reader->cursor, BR_CHUNK_BYTES);
    }
    xSemaphoreGive(reader->lock);
    xSemaphoreGive(reader->wake);
}
