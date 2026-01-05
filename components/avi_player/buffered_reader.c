#include "buffered_reader.h"
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <assert.h>
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "freertos/idf_additions.h"

static const char *TAG = "buffered_reader";
#define LOG_ERROR(fmt, ...) ESP_LOGE(TAG, fmt, ##__VA_ARGS__)
#define LOG_INFO(fmt, ...) ESP_LOGI(TAG, fmt, ##__VA_ARGS__)
#define LOG_DEBUG(fmt, ...)
// #define LOG_DEBUG(fmt, ...) printf("[BR] " fmt "\n", ##__VA_ARGS__)

static void *memory_allocate(size_t size) { return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED); }
static void memory_free(void *ptr) { return heap_caps_free(ptr); }

typedef enum {
    BR_EVENT_ACTIVE = 1 << 0,
    BR_EVENT_STOP   = 1 << 1,
    BR_EVENT_MAX    = 1 << 2,
} br_event_t;
#define BR_EVENT_ALL (BR_EVENT_MAX - 1)

#define BR_CHUNK_IDX_UNUSED (UINT32_MAX)
typedef struct buffered_reader {
    int fd;
    SemaphoreHandle_t mutex;
    EventGroupHandle_t event_group;
    off_t file_size;
    off_t first_chunk_offset;
    off_t current_offset;
    bool preload_enabled;
    uint8_t chunk_offset;
    uint8_t chunk_length;
    uint8_t *buffer[BR_CHUNK_NUM];
} buffered_reader_t;

static void br_preload_task(void *args) {
    LOG_INFO("Start Preload Task");
    buffered_reader_t *reader = (buffered_reader_t*)args;
    while (true) {
        br_event_t event = xEventGroupWaitBits(reader->event_group, BR_EVENT_ALL, 0, 0, portMAX_DELAY);
        if (event & BR_EVENT_STOP) break;

        xSemaphoreTake(reader->mutex, portMAX_DELAY);
        off_t current_offset = reader->current_offset;
        LOG_DEBUG("current_offset: 0x%08lX, first_chunk_offset: 0x%08lX", current_offset, reader->first_chunk_offset);
        if (current_offset < reader->first_chunk_offset) {
            reader->chunk_offset = 0;
            reader->chunk_length = 0;
            reader->first_chunk_offset = 0;
        } else if (reader->chunk_length > 0 && reader->first_chunk_offset + BR_CHUNK_SIZE <= current_offset) {
            LOG_DEBUG("invalidate chunk: 0x%08lX", reader->first_chunk_offset);
            while (reader->chunk_length > 0 && reader->first_chunk_offset + BR_CHUNK_SIZE <= current_offset) {
                reader->first_chunk_offset += BR_CHUNK_SIZE;
                reader->chunk_offset = (reader->chunk_offset + 1) % BR_CHUNK_NUM;
                reader->chunk_length--;
            }
        } else if (reader->chunk_length < BR_CHUNK_NUM - 1) {
            int chunk_index;
            off_t file_offset;
            if (reader->chunk_length > 0 && reader->first_chunk_offset <= reader->current_offset) {
                chunk_index = (reader->chunk_offset + reader->chunk_length) % BR_CHUNK_NUM;
                file_offset = reader->first_chunk_offset + reader->chunk_length * BR_CHUNK_SIZE;
                LOG_DEBUG("preload first chunk: 0x%08lX, chunk_index=%d", file_offset, chunk_index);
            } else {
                chunk_index = 0;
                file_offset = current_offset & ~(BR_CHUNK_SIZE - 1);
                reader->chunk_offset = 0;
                reader->first_chunk_offset = file_offset;
                LOG_DEBUG("preload first chunk: 0x%08lX", file_offset);
            }
            size_t read_size = file_offset + BR_CHUNK_SIZE <= reader->file_size ? BR_CHUNK_SIZE : reader->file_size - file_offset;
            lseek(reader->fd, file_offset, SEEK_SET);
            size_t result = read(reader->fd, reader->buffer[chunk_index], read_size);
            if (result == read_size) reader->chunk_length++;
        }
        xSemaphoreGive(reader->mutex);
    }
    vEventGroupDelete(reader->event_group);
    reader->event_group = NULL;
    LOG_INFO("End Preload Task");
    vTaskDelete(NULL);
}

buffered_reader_t *br_open(const char *path) {
    // Open file
    int fd = open(path, O_RDONLY);
    if (!fd) return NULL;
    buffered_reader_t *reader = (buffered_reader_t*)memory_allocate(sizeof(buffered_reader_t));
    reader->fd = fd;

    // Create Mutex / Event Group / Task
    reader->mutex = xSemaphoreCreateMutex();
    assert(reader->mutex);
    reader->event_group = xEventGroupCreate();
    assert(reader->event_group);
    xTaskCreatePinnedToCore(br_preload_task, "preload", 4096, reader, 1, NULL, 0);

    // Get File Size
    struct stat st;
    if (fstat(fd, &st)) { assert(false); }
    reader->file_size = st.st_size;

    // intialize buffers
    reader->first_chunk_offset = 0;
    reader->current_offset = 0;
    reader->preload_enabled = false;
    reader->chunk_offset = 0;
    reader->chunk_length = 0;
    for (int i = 0; i < BR_CHUNK_NUM; i++) {
        reader->buffer[i] = memory_allocate(BR_CHUNK_SIZE);
        assert(reader->buffer[i]);
    }
    return reader;
}

void br_close(buffered_reader_t *reader) {
    xEventGroupSetBits(reader->event_group, BR_EVENT_STOP);
    while (reader->event_group) vTaskDelay(pdMS_TO_TICKS(10));
    vSemaphoreDelete(reader->mutex);
    close(reader->fd);
    for (int i = 0; i < BR_CHUNK_NUM; i++) memory_free(reader->buffer[i]);
    memory_free(reader);
}

size_t br_read(buffered_reader_t *reader, void *buffer, size_t size) {
    if (!reader->preload_enabled) {
        size_t result = read(reader->fd, buffer, size);
        reader->current_offset += result;
        return result;
    }

    off_t current_offset = reader->current_offset;
    off_t first_chunk_offset = reader->first_chunk_offset;
    off_t last_chunk_offset = first_chunk_offset + BR_CHUNK_SIZE * reader->chunk_length;
    if (current_offset + size > reader->file_size) size = reader->file_size - current_offset;
    off_t last_offset = current_offset + size;

    if (current_offset < first_chunk_offset || last_chunk_offset < last_offset) {
        xSemaphoreTake(reader->mutex, portMAX_DELAY);
        first_chunk_offset = reader->first_chunk_offset;
        last_chunk_offset = first_chunk_offset + BR_CHUNK_SIZE * reader->chunk_length;
        if (current_offset < first_chunk_offset || last_chunk_offset < last_offset) {
            lseek(reader->fd, reader->current_offset, SEEK_SET);
            size_t result = read(reader->fd, buffer, size);
            reader->current_offset += result;
            LOG_INFO("preload miss read: size=0x%08X, offset=0x%08lX, first_chunk_offset=0x%08lX, chunk_length=%d",
                result, reader->current_offset, first_chunk_offset, reader->chunk_length);
            xSemaphoreGive(reader->mutex);
            vTaskDelay(pdMS_TO_TICKS(5000));
            LOG_INFO("buffered: first_chunk_offset=0x%08lX, chunk_length=%d", first_chunk_offset, reader->chunk_length);
            return result;
        }
        xSemaphoreGive(reader->mutex);
    }

    uint8_t *p = (uint8_t*)buffer;
    size_t remaining = size;

    for (int i = 0; i < reader->chunk_length; i++) {
        // i番目のチャンクの範囲を計算
        off_t chunk_start = first_chunk_offset + i * BR_CHUNK_SIZE;
        off_t chunk_end = chunk_start + BR_CHUNK_SIZE;

        // 現在のカーソル位置がこのチャンクの範囲外なら次へ
        if (current_offset >= chunk_end) continue;
        if (current_offset + remaining <= chunk_start) break;

        // 循環バッファ内のインデックスを計算
        int buffer_index = (reader->chunk_offset + i) % BR_CHUNK_NUM;

        // このチャンク内での読み取り開始位置（チャンク先頭からのオフセット）
        int chunk_offset = (current_offset > chunk_start) ? (current_offset - chunk_start) : 0;

        // このチャンクから読み取るバイト数を計算
        int bytes_to_copy = BR_CHUNK_SIZE - chunk_offset;
        if (bytes_to_copy > remaining) bytes_to_copy = remaining;

        // データをコピー
        memcpy(p, &reader->buffer[buffer_index][chunk_offset], bytes_to_copy);

        // ポインタと残りバイト数を更新
        p += bytes_to_copy;
        current_offset += bytes_to_copy;
        remaining -= bytes_to_copy;
    }
    reader->current_offset = current_offset;
    // LOG_DEBUG("buffer read: size=0x%08X, offset=0x%08lX", size, reader->current_offset);
    return size;
}

off_t br_lseek(buffered_reader_t *reader, off_t offset, int whence) {
    if (!reader->preload_enabled) {
        reader->current_offset = lseek(reader->fd, offset, whence);
        return reader->current_offset;
    }
    // LOG_DEBUG("seek: current=0x%08lX, offset=0x%08lX, whence=%d", reader->current_offset, offset, whence);
    switch (whence) {
    case SEEK_SET:
        reader->current_offset = offset;
        break;
    case SEEK_CUR:
        reader->current_offset += offset;
        break;
    case SEEK_END:
        reader->current_offset = reader->file_size + offset;
        break;
    }
    return reader->current_offset;
}

void br_set_preload_enable(buffered_reader_t *reader, bool enable) {
    if (enable) {
        xSemaphoreTake(reader->mutex, portMAX_DELAY);
        reader->current_offset = lseek(reader->fd, 0, SEEK_CUR);
        xEventGroupSetBits(reader->event_group, BR_EVENT_ACTIVE);
        reader->preload_enabled = true;
        LOG_DEBUG("Prefetch enable: 0x%08lX", reader->current_offset);
        xSemaphoreGive(reader->mutex);
        vTaskDelay(pdMS_TO_TICKS(100));
    } else {
        xSemaphoreTake(reader->mutex, portMAX_DELAY);
        lseek(reader->fd, reader->current_offset, SEEK_SET);
        xEventGroupClearBits(reader->event_group, BR_EVENT_ACTIVE);
        reader->preload_enabled = false;
        LOG_DEBUG("Prefetch disable: 0x%08lX", reader->current_offset);
        xSemaphoreGive(reader->mutex);
    }
}
