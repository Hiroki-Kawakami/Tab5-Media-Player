/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "media_buffer.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "media_buffer";

#define MB_CHUNK_BYTES (64 * 1024)
#define MB_DEPTH_CHUNKS 16
#define MB_WAIT_MS 20
#define MB_WINDOW_BYTES (64 * 1024)
#define MB_STOP_TIMEOUT_MS 2000
#define MB_BOUNCE_REF 0xFFFFFFFFu

struct media_buffer {
    int fd;
    off_t size;
    off_t cursor;
    off_t base;
    int head;
    int filled;
    int want;
    bool readahead;
    bool direct;
    off_t win_base;
    size_t win_len;
    bool interrupted;
    bool io_error;
    bool running;
    volatile bool stop;
    SemaphoreHandle_t lock;
    SemaphoreHandle_t io_lock;
    SemaphoreHandle_t wake;
    SemaphoreHandle_t data;
    SemaphoreHandle_t done;
    uint8_t *ring;
    int ring_chunks;
    int depth;
    uint16_t *pins;
    uint8_t *bounce;
    size_t bounce_bytes;
    int bounce_pins;
};

static size_t bounce_bytes_for(size_t arena_bytes) {
    return arena_bytes / 4 / MB_CHUNK_BYTES * MB_CHUNK_BYTES;
}

size_t mb_arena_max_view(const media_arena_t *arena) {
    return arena && arena->data ? bounce_bytes_for(arena->size) : 0;
}

static off_t align_down(off_t value, off_t alignment) {
    return value - (value % alignment);
}

static uint8_t *chunk_at(const media_buffer_t *buffer, int index) {
    return buffer->ring + (size_t)((buffer->head + index) % buffer->ring_chunks) * MB_CHUNK_BYTES;
}

static size_t read_at(media_buffer_t *buffer, off_t offset, void *out, size_t size) {
    xSemaphoreTake(buffer->io_lock, portMAX_DELAY);
    size_t got = 0;
    if (lseek(buffer->fd, offset, SEEK_SET) == offset) {
        while (got < size) {
            const ssize_t n = read(buffer->fd, (uint8_t *)out + got, size - got);
            if (n <= 0) break;
            got += (size_t)n;
        }
    }
    xSemaphoreGive(buffer->io_lock);
    return got;
}

static void restart_window(media_buffer_t *buffer) {
    buffer->head = (buffer->head + buffer->filled) % buffer->ring_chunks;
    buffer->filled = 0;
    buffer->base = align_down(buffer->cursor, MB_CHUNK_BYTES);
}

static void drop_consumed(media_buffer_t *buffer) {
    if (buffer->cursor < buffer->base) {
        restart_window(buffer);
        return;
    }
    while (buffer->filled > 0 && buffer->base + MB_CHUNK_BYTES <= buffer->cursor) {
        buffer->base += MB_CHUNK_BYTES;
        buffer->head = (buffer->head + 1) % buffer->ring_chunks;
        buffer->filled--;
    }
    if (buffer->filled == 0) buffer->base = align_down(buffer->cursor, MB_CHUNK_BYTES);
}

static bool wait_for(media_buffer_t *buffer, off_t end) {
    for (;;) {
        drop_consumed(buffer);
        if (end <= buffer->base + (off_t)buffer->filled * MB_CHUNK_BYTES) return true;
        if (!buffer->readahead || buffer->io_error || buffer->interrupted) return false;
        const int need = (int)((end - buffer->base + MB_CHUNK_BYTES - 1) / MB_CHUNK_BYTES);
        if (need > buffer->want) buffer->want = need;
        xSemaphoreGive(buffer->lock);
        xSemaphoreGive(buffer->wake);
        xSemaphoreTake(buffer->data, pdMS_TO_TICKS(MB_WAIT_MS));
        xSemaphoreTake(buffer->lock, portMAX_DELAY);
    }
}

static void readahead_task(void *arg) {
    media_buffer_t *buffer = (media_buffer_t *)arg;

    while (!buffer->stop) {
        off_t offset = 0;
        int slot = -1;

        xSemaphoreTake(buffer->lock, portMAX_DELAY);
        if (buffer->readahead && !buffer->io_error) {
            drop_consumed(buffer);
            const int depth = buffer->want > buffer->depth ? buffer->want : buffer->depth;
            offset = buffer->base + (off_t)buffer->filled * MB_CHUNK_BYTES;
            if (buffer->filled < depth && offset < buffer->size) {
                const int next = (buffer->head + buffer->filled) % buffer->ring_chunks;
                if (buffer->pins[next] == 0) slot = next;
            }
        }
        xSemaphoreGive(buffer->lock);

        if (slot < 0) {
            xSemaphoreTake(buffer->wake, pdMS_TO_TICKS(MB_WAIT_MS));
            continue;
        }

        size_t want = MB_CHUNK_BYTES;
        if (offset + (off_t)want > buffer->size) want = (size_t)(buffer->size - offset);
        const size_t got = read_at(buffer, offset,
                                   buffer->ring + (size_t)slot * MB_CHUNK_BYTES, want);

        xSemaphoreTake(buffer->lock, portMAX_DELAY);
        if (buffer->readahead &&
            buffer->base + (off_t)buffer->filled * MB_CHUNK_BYTES == offset &&
            (buffer->head + buffer->filled) % buffer->ring_chunks == slot) {
            if (got == want) {
                buffer->filled++;
            } else {
                ESP_LOGW(TAG, "short read at %ld: %u of %u", (long)offset, (unsigned)got,
                         (unsigned)want);
                buffer->io_error = true;
            }
        }
        xSemaphoreGive(buffer->lock);
        xSemaphoreGive(buffer->data);
    }

    xSemaphoreGive(buffer->done);
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(NULL);
#else
    vTaskDelete(NULL);
#endif
}

media_buffer_t *mb_open(const char *path, const media_arena_t *arena) {
    const size_t bounce_bytes = arena ? bounce_bytes_for(arena->size) : 0;
    const int bounce_chunks = (int)(bounce_bytes / MB_CHUNK_BYTES);
    const int ring_chunks = arena ? (int)((arena->size - bounce_bytes) / MB_CHUNK_BYTES) : 0;
    const int depth = ring_chunks / 2 < MB_DEPTH_CHUNKS ? ring_chunks / 2 : MB_DEPTH_CHUNKS;
    const int window = bounce_chunks + 1 > depth ? bounce_chunks + 1 : depth;
    if (!arena || !arena->data || (uintptr_t)arena->data % MB_ARENA_ALIGNMENT != 0 ||
        bounce_chunks == 0 || ring_chunks < window * 2) {
        ESP_LOGE(TAG, "unusable arena");
        return NULL;
    }

    const int fd = open(path, O_RDONLY);
    if (fd < 0) {
        ESP_LOGE(TAG, "open %s: errno %d", path, errno);
        return NULL;
    }

    media_buffer_t *buffer = heap_caps_calloc(1, sizeof(*buffer), MALLOC_CAP_SPIRAM);
    if (!buffer) buffer = heap_caps_calloc(1, sizeof(*buffer), MALLOC_CAP_DEFAULT);
    if (!buffer) {
        ESP_LOGE(TAG, "no memory for the buffer");
        close(fd);
        return NULL;
    }
    buffer->fd = fd;

    struct stat info;
    if (fstat(fd, &info) != 0) {
        mb_close(buffer);
        return NULL;
    }
    buffer->size = info.st_size;
    buffer->ring = arena->data;
    buffer->ring_chunks = ring_chunks;
    buffer->depth = depth;
    buffer->direct = arena->direct;
    buffer->bounce = arena->data + (size_t)ring_chunks * MB_CHUNK_BYTES;
    buffer->bounce_bytes = bounce_bytes;

    buffer->pins = heap_caps_calloc((size_t)ring_chunks, sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!buffer->pins) {
        buffer->pins = heap_caps_calloc((size_t)ring_chunks, sizeof(uint16_t), MALLOC_CAP_DEFAULT);
    }
    buffer->lock = xSemaphoreCreateMutex();
    buffer->io_lock = xSemaphoreCreateMutex();
    buffer->wake = xSemaphoreCreateBinary();
    buffer->data = xSemaphoreCreateBinary();
    buffer->done = xSemaphoreCreateBinary();
    if (!buffer->pins || !buffer->lock || !buffer->io_lock || !buffer->wake || !buffer->data ||
        !buffer->done) {
        ESP_LOGE(TAG, "no memory for the buffer objects");
        mb_close(buffer);
        return NULL;
    }

#ifdef ESP_PLATFORM
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        readahead_task, "media_readahead", 3072, buffer, 3, NULL, 0, MALLOC_CAP_SPIRAM);
#else
    const BaseType_t created =
        xTaskCreatePinnedToCore(readahead_task, "media_readahead", 3072, buffer, 3, NULL, 0);
#endif
    if (created != pdPASS) {
        ESP_LOGE(TAG, "no memory for the read-ahead task");
        mb_close(buffer);
        return NULL;
    }
    buffer->running = true;
    return buffer;
}

void mb_close(media_buffer_t *buffer) {
    if (!buffer) return;

    if (buffer->running) {
        buffer->stop = true;
        xSemaphoreGive(buffer->wake);
        if (xSemaphoreTake(buffer->done, pdMS_TO_TICKS(MB_STOP_TIMEOUT_MS)) != pdTRUE) {
            ESP_LOGE(TAG, "read-ahead did not stop; leaking the buffer");
            return;
        }
        buffer->running = false;
    }

    heap_caps_free(buffer->pins);
    if (buffer->lock) vSemaphoreDelete(buffer->lock);
    if (buffer->io_lock) vSemaphoreDelete(buffer->io_lock);
    if (buffer->wake) vSemaphoreDelete(buffer->wake);
    if (buffer->data) vSemaphoreDelete(buffer->data);
    if (buffer->done) vSemaphoreDelete(buffer->done);
    close(buffer->fd);
    heap_caps_free(buffer);
}

off_t mb_size(const media_buffer_t *buffer) { return buffer->size; }
off_t mb_tell(const media_buffer_t *buffer) { return buffer->cursor; }

static size_t read_windowed(media_buffer_t *buffer, void *out, size_t size) {
    const size_t window = buffer->bounce_bytes < MB_WINDOW_BYTES ? buffer->bounce_bytes
                                                                 : MB_WINDOW_BYTES;
    size_t done = 0;
    while (done < size) {
        const off_t cursor = buffer->cursor;
        if (size - done >= window) {
            const size_t got = read_at(buffer, cursor, (uint8_t *)out + done, size - done);
            buffer->cursor += (off_t)got;
            return done + got;
        }
        if (!buffer->win_len || cursor < buffer->win_base ||
            cursor >= buffer->win_base + (off_t)buffer->win_len) {
            buffer->win_base = cursor;
            buffer->win_len = read_at(buffer, buffer->win_base, buffer->bounce, window);
            if (!buffer->win_len) break;
        }
        const size_t offset = (size_t)(cursor - buffer->win_base);
        size_t take = buffer->win_len - offset;
        if (take > size - done) take = size - done;
        memcpy((uint8_t *)out + done, buffer->bounce + offset, take);
        buffer->cursor += (off_t)take;
        done += take;
    }
    return done;
}

size_t mb_read(media_buffer_t *buffer, void *out, size_t size) {
    if (buffer->cursor >= buffer->size) return 0;
    if (buffer->cursor + (off_t)size > buffer->size) size = (size_t)(buffer->size - buffer->cursor);

    xSemaphoreTake(buffer->lock, portMAX_DELAY);
    if (buffer->direct) {
        const size_t got = read_windowed(buffer, out, size);
        xSemaphoreGive(buffer->lock);
        return got;
    }
    if (!buffer->readahead) {
        const off_t offset = buffer->cursor;
        xSemaphoreGive(buffer->lock);
        const size_t got = read_at(buffer, offset, out, size);
        xSemaphoreTake(buffer->lock, portMAX_DELAY);
        buffer->cursor += got;
        xSemaphoreGive(buffer->lock);
        return got;
    }

    size_t done = 0;
    while (done < size) {
        off_t end = align_down(buffer->cursor, MB_CHUNK_BYTES) + MB_CHUNK_BYTES;
        if (end > buffer->cursor + (off_t)(size - done)) end = buffer->cursor + (off_t)(size - done);
        if (!wait_for(buffer, end)) break;
        const int index = (int)((buffer->cursor - buffer->base) / MB_CHUNK_BYTES);
        const size_t offset = (size_t)((buffer->cursor - buffer->base) % MB_CHUNK_BYTES);
        const size_t take = (size_t)(end - buffer->cursor);
        memcpy((uint8_t *)out + done, chunk_at(buffer, index) + offset, take);
        buffer->cursor += (off_t)take;
        done += take;
    }
    buffer->want = 0;
    xSemaphoreGive(buffer->lock);
    xSemaphoreGive(buffer->wake);
    return done;
}

static const uint8_t *view_locked(media_buffer_t *buffer, off_t start, size_t size, bool advance,
                                  uint32_t *ref) {
    const off_t end = start + (off_t)size;
    if (!buffer->readahead || !wait_for(buffer, end)) {
        buffer->want = 0;
        xSemaphoreGive(buffer->lock);
        return NULL;
    }

    const int first = (int)((start - buffer->base) / MB_CHUNK_BYTES);
    const size_t offset = (size_t)((start - buffer->base) % MB_CHUNK_BYTES);
    const int last = (int)((end - 1 - buffer->base) / MB_CHUNK_BYTES);
    const int slot = (buffer->head + first) % buffer->ring_chunks;
    const int count = last - first + 1;

    if (slot + count <= buffer->ring_chunks) {
        for (int i = 0; i < count; i++) buffer->pins[slot + i]++;
        if (advance) buffer->cursor = end;
        buffer->want = 0;
        xSemaphoreGive(buffer->lock);
        xSemaphoreGive(buffer->wake);
        *ref = ((uint32_t)slot << 16) | (uint32_t)count;
        return buffer->ring + (size_t)slot * MB_CHUNK_BYTES + offset;
    }

    while (buffer->bounce_pins > 0) {
        if (buffer->interrupted) {
            buffer->want = 0;
            xSemaphoreGive(buffer->lock);
            return NULL;
        }
        xSemaphoreGive(buffer->lock);
        xSemaphoreTake(buffer->data, pdMS_TO_TICKS(MB_WAIT_MS));
        xSemaphoreTake(buffer->lock, portMAX_DELAY);
    }
    buffer->bounce_pins = 1;
    const off_t base = buffer->base;
    const int head = buffer->head;
    xSemaphoreGive(buffer->lock);

    size_t done = 0;
    while (done < size) {
        const off_t position = start + (off_t)done;
        const int index = (int)((position - base) / MB_CHUNK_BYTES);
        const size_t from = (size_t)((position - base) % MB_CHUNK_BYTES);
        size_t take = MB_CHUNK_BYTES - from;
        if (take > size - done) take = size - done;
        const uint8_t *chunk =
            buffer->ring + (size_t)((head + index) % buffer->ring_chunks) * MB_CHUNK_BYTES;
        memcpy(buffer->bounce + done, chunk + from, take);
        done += take;
    }

    xSemaphoreTake(buffer->lock, portMAX_DELAY);
    if (advance) buffer->cursor = end;
    buffer->want = 0;
    xSemaphoreGive(buffer->lock);
    xSemaphoreGive(buffer->wake);
    *ref = MB_BOUNCE_REF;
    return buffer->bounce;
}

const uint8_t *mb_view(media_buffer_t *buffer, size_t size, uint32_t *ref) {
    *ref = MB_NO_REF;
    if (size == 0 || size > buffer->bounce_bytes) return NULL;
    if (buffer->cursor + (off_t)size > buffer->size) return NULL;

    xSemaphoreTake(buffer->lock, portMAX_DELAY);
    return view_locked(buffer, buffer->cursor, size, true, ref);
}

const uint8_t *mb_view_at(media_buffer_t *buffer, off_t offset, size_t size, uint32_t *ref) {
    *ref = MB_NO_REF;
    if (offset < 0 || size == 0 || size > buffer->bounce_bytes) return NULL;
    if (offset + (off_t)size > buffer->size) return NULL;

    xSemaphoreTake(buffer->lock, portMAX_DELAY);
    const off_t span = offset + (off_t)size - align_down(buffer->cursor, MB_CHUNK_BYTES);
    if (offset < buffer->cursor || span > (off_t)(buffer->ring_chunks / 2) * MB_CHUNK_BYTES) {
        buffer->cursor = offset;
        buffer->io_error = false;
        drop_consumed(buffer);
    }
    return view_locked(buffer, offset, size, false, ref);
}

void mb_release(media_buffer_t *buffer, uint32_t ref) {
    if (!buffer || ref == MB_NO_REF) return;
    xSemaphoreTake(buffer->lock, portMAX_DELAY);
    if (ref == MB_BOUNCE_REF) {
        if (buffer->bounce_pins > 0) buffer->bounce_pins--;
    } else {
        const int slot = (int)(ref >> 16);
        const int count = (int)(ref & 0xFFFF);
        for (int i = 0; i < count && slot + i < buffer->ring_chunks; i++) {
            if (buffer->pins[slot + i] > 0) buffer->pins[slot + i]--;
        }
    }
    xSemaphoreGive(buffer->lock);
    xSemaphoreGive(buffer->wake);
    xSemaphoreGive(buffer->data);
}

void mb_release_all(media_buffer_t *buffer) {
    if (!buffer) return;
    xSemaphoreTake(buffer->lock, portMAX_DELAY);
    memset(buffer->pins, 0, (size_t)buffer->ring_chunks * sizeof(uint16_t));
    buffer->bounce_pins = 0;
    xSemaphoreGive(buffer->lock);
    xSemaphoreGive(buffer->wake);
}

void mb_interrupt(media_buffer_t *buffer, bool interrupted) {
    if (!buffer) return;
    xSemaphoreTake(buffer->lock, portMAX_DELAY);
    buffer->interrupted = interrupted;
    xSemaphoreGive(buffer->lock);
    xSemaphoreGive(buffer->data);
}

off_t mb_seek(media_buffer_t *buffer, off_t offset) {
    if (offset < 0) offset = 0;
    if (offset > buffer->size) offset = buffer->size;
    xSemaphoreTake(buffer->lock, portMAX_DELAY);
    buffer->cursor = offset;
    buffer->io_error = false;
    drop_consumed(buffer);
    xSemaphoreGive(buffer->lock);
    xSemaphoreGive(buffer->wake);
    return offset;
}

void mb_skip(media_buffer_t *buffer, off_t delta) {
    mb_seek(buffer, buffer->cursor + delta);
}

void mb_set_readahead(media_buffer_t *buffer, bool enabled) {
    if (buffer->direct) return;
    xSemaphoreTake(buffer->lock, portMAX_DELAY);
    buffer->readahead = enabled;
    if (!enabled) restart_window(buffer);
    xSemaphoreGive(buffer->lock);
    xSemaphoreGive(buffer->wake);
}
