/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "file_operation.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "file_operation";

static constexpr uint32_t kStackBytes = 8192;
static constexpr UBaseType_t kPriority = 2;
static constexpr uint32_t kReaderStackBytes = 4096;
static constexpr std::size_t kCopyBufferBytes = 1024 * 1024;
static constexpr std::size_t kCopyBuffers = 2;
static constexpr std::size_t kBufferAlignment = 64;

struct Chunk {
    uint8_t *data;
    ssize_t length;
    int error;
};

static std::mutex s_mutex;
static FileOperationStatus s_status;
static std::vector<std::string> s_deletes;
static std::vector<FileCopy> s_copies;
static uint8_t *s_buffers[kCopyBuffers];
static QueueHandle_t s_requests;
static QueueHandle_t s_free;
static QueueHandle_t s_filled;
static SemaphoreHandle_t s_reader_exit;
static std::atomic<bool> s_stop_read;
static std::atomic<bool> s_running;
static std::atomic<bool> s_cancel;

static void set_current(const std::string &path) {
    std::lock_guard<std::mutex> lock(s_mutex);
    s_status.current = path.substr(path.rfind('/') + 1);
}

static bool fail(const char *what, const std::string &path) {
    const int error = errno;
    ESP_LOGE(TAG, "%s %s: %s", what, path.c_str(), strerror(error));
    std::lock_guard<std::mutex> lock(s_mutex);
    s_status.failed_path = path;
    s_status.error = error;
    return false;
}

static bool is_directory(const std::string &path, bool *directory) {
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return fail("stat", path);
    *directory = S_ISDIR(st.st_mode);
    return true;
}

static bool remove_entry(std::string &path, bool directory) {
    if (s_cancel) return false;
    set_current(path);
    if (!directory) return unlink(path.c_str()) == 0 || fail("unlink", path);

    DIR *dir = opendir(path.c_str());
    if (!dir) return fail("opendir", path);
    bool ok = true;
    while (struct dirent *ent = readdir(dir)) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        const std::size_t length = path.size();
        path += '/';
        path += ent->d_name;
        ok = remove_entry(path, ent->d_type == DT_DIR);
        path.resize(length);
        if (!ok) break;
    }
    closedir(dir);
    if (!ok) return false;
    return rmdir(path.c_str()) == 0 || fail("rmdir", path);
}

static bool measure(std::string &path, bool directory, uint64_t *bytes) {
    if (s_cancel) return false;
    if (!directory) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) return fail("stat", path);
        *bytes += st.st_size;
        return true;
    }
    DIR *dir = opendir(path.c_str());
    if (!dir) return fail("opendir", path);
    bool ok = true;
    while (struct dirent *ent = readdir(dir)) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        const std::size_t length = path.size();
        path += '/';
        path += ent->d_name;
        ok = measure(path, ent->d_type == DT_DIR, bytes);
        path.resize(length);
        if (!ok) break;
    }
    closedir(dir);
    return ok;
}

static void reader_task(void *) {
    int fd;
    while (xQueueReceive(s_requests, &fd, portMAX_DELAY) == pdTRUE && fd >= 0) {
        while (true) {
            Chunk chunk = {};
            xQueueReceive(s_free, &chunk.data, portMAX_DELAY);
            if (!s_stop_read && !s_cancel) {
                chunk.length = read(fd, chunk.data, kCopyBufferBytes);
                if (chunk.length < 0) chunk.error = errno;
            }
            xQueueSend(s_filled, &chunk, portMAX_DELAY);
            if (chunk.length <= 0) break;
        }
    }
    xSemaphoreGive(s_reader_exit);
    vTaskDelete(nullptr);
}

static bool copy_file(const std::string &source, const std::string &target) {
    set_current(source);
    const int in = open(source.c_str(), O_RDONLY);
    if (in < 0) return fail("open", source);
    const int out = open(target.c_str(), O_WRONLY | O_CREAT | O_EXCL, 0666);
    if (out < 0) {
        close(in);
        return fail("create", target);
    }
    bool ok = true;
    s_stop_read = false;
    xQueueSend(s_requests, &in, portMAX_DELAY);
    while (true) {
        Chunk chunk;
        xQueueReceive(s_filled, &chunk, portMAX_DELAY);
        if (chunk.length < 0 && ok) {
            errno = chunk.error;
            ok = fail("read", source);
        }
        if (chunk.length > 0 && ok) {
            for (ssize_t written = 0; ok && written < chunk.length;) {
                const ssize_t n = write(out, chunk.data + written, chunk.length - written);
                if (n <= 0) {
                    if (n == 0) errno = ENOSPC;
                    ok = fail("write", target);
                } else {
                    written += n;
                }
            }
            if (!ok) s_stop_read = true;
            std::lock_guard<std::mutex> lock(s_mutex);
            if (ok) s_status.bytes_done += chunk.length;
        }
        xQueueSend(s_free, &chunk.data, portMAX_DELAY);
        if (chunk.length <= 0) break;
    }
    if (s_cancel) ok = false;
    close(in);
    if (close(out) != 0 && ok) ok = fail("close", target);
    if (!ok) unlink(target.c_str());
    return ok;
}

static bool copy_entry(std::string &source, std::string &target, bool directory) {
    if (s_cancel) return false;
    if (!directory) return copy_file(source, target);

    set_current(source);
    if (mkdir(target.c_str(), 0777) != 0) return fail("mkdir", target);
    DIR *dir = opendir(source.c_str());
    if (!dir) return fail("opendir", source);
    bool ok = true;
    while (struct dirent *ent = readdir(dir)) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0) continue;
        const std::size_t source_length = source.size();
        const std::size_t target_length = target.size();
        source += '/';
        source += ent->d_name;
        target += '/';
        target += ent->d_name;
        ok = copy_entry(source, target, ent->d_type == DT_DIR);
        source.resize(source_length);
        target.resize(target_length);
        if (!ok) break;
    }
    closedir(dir);
    return ok;
}

static void run_delete() {
    for (std::string &path : s_deletes) {
        bool directory;
        if (!is_directory(path, &directory) || !remove_entry(path, directory)) break;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.done++;
    }
}

static void run_copy() {
    std::vector<bool> directories;
    uint64_t bytes = 0;
    for (FileCopy &copy : s_copies) {
        bool directory;
        if (!is_directory(copy.source, &directory) || !measure(copy.source, directory, &bytes)) {
            return;
        }
        directories.push_back(directory);
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.bytes_total = bytes;
    }
    s_requests = xQueueCreate(1, sizeof(int));
    s_free = xQueueCreate(kCopyBuffers, sizeof(uint8_t *));
    s_filled = xQueueCreate(kCopyBuffers, sizeof(Chunk));
    s_reader_exit = xSemaphoreCreateBinary();
    if (!s_requests || !s_free || !s_filled || !s_reader_exit ||
        xTaskCreate(reader_task, "file_read", kReaderStackBytes, nullptr, kPriority, nullptr) != pdPASS) {
        errno = ENOMEM;
        fail("reader", s_copies.front().source);
    } else {
        for (uint8_t *buffer : s_buffers) xQueueSend(s_free, &buffer, 0);
        for (std::size_t i = 0; i < s_copies.size(); i++) {
            if (!copy_entry(s_copies[i].source, s_copies[i].target, directories[i])) break;
            std::lock_guard<std::mutex> lock(s_mutex);
            s_status.done++;
        }
        const int stop = -1;
        xQueueSend(s_requests, &stop, portMAX_DELAY);
        xSemaphoreTake(s_reader_exit, portMAX_DELAY);
    }
    if (s_requests) vQueueDelete(s_requests);
    if (s_free) vQueueDelete(s_free);
    if (s_filled) vQueueDelete(s_filled);
    if (s_reader_exit) vSemaphoreDelete(s_reader_exit);
    s_requests = s_free = s_filled = nullptr;
    s_reader_exit = nullptr;
}

static void free_buffers() {
    for (uint8_t *&buffer : s_buffers) {
        heap_caps_free(buffer);
        buffer = nullptr;
    }
}

static void operation_task(void *) {
    if (!s_deletes.empty()) {
        run_delete();
    } else {
        run_copy();
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.cancelled = s_cancel && s_status.error == 0;
        s_status.finished = true;
    }
    s_deletes.clear();
    s_copies.clear();
    free_buffers();
    s_running = false;
    vTaskDelete(nullptr);
}

static bool start(std::size_t total) {
    s_cancel = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status = {};
        s_status.total = total;
    }
    if (xTaskCreate(operation_task, "file_op", kStackBytes, nullptr, kPriority, nullptr) == pdPASS) {
        return true;
    }
    ESP_LOGE(TAG, "task create failed");
    s_deletes.clear();
    s_copies.clear();
    free_buffers();
    s_running = false;
    return false;
}

bool file_operation_delete(std::vector<std::string> paths) {
    if (paths.empty() || s_running.exchange(true)) return false;
    s_deletes = std::move(paths);
    return start(s_deletes.size());
}

bool file_operation_copy(std::vector<FileCopy> copies) {
    if (copies.empty() || s_running.exchange(true)) return false;
    for (uint8_t *&buffer : s_buffers) {
        buffer = static_cast<uint8_t *>(
            heap_caps_aligned_alloc(kBufferAlignment, kCopyBufferBytes, MALLOC_CAP_SPIRAM));
        if (buffer) continue;
        ESP_LOGE(TAG, "no memory for the copy buffers");
        free_buffers();
        s_running = false;
        return false;
    }
    s_copies = std::move(copies);
    return start(s_copies.size());
}

FileOperationStatus file_operation_status() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_status;
}

void file_operation_cancel() {
    s_cancel = true;
}
