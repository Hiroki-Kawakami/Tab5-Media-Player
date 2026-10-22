/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "file_operation.hpp"

#include <atomic>
#include <cerrno>
#include <cstring>
#include <dirent.h>
#include <mutex>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "file_operation";

static constexpr uint32_t kStackBytes = 8192;
static constexpr UBaseType_t kPriority = 2;

static std::mutex s_mutex;
static FileOperationStatus s_status;
static std::vector<std::string> s_paths;
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

static void delete_task(void *) {
    for (std::string &path : s_paths) {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            fail("stat", path);
            break;
        }
        if (!remove_entry(path, S_ISDIR(st.st_mode))) break;
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.done++;
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status.cancelled = s_cancel && s_status.error == 0;
        s_status.finished = true;
    }
    s_paths.clear();
    s_running = false;
    vTaskDelete(nullptr);
}

bool file_operation_delete(std::vector<std::string> paths) {
    if (s_running.exchange(true)) return false;
    s_paths = std::move(paths);
    s_cancel = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        s_status = {};
        s_status.total = s_paths.size();
    }
    if (xTaskCreate(delete_task, "file_op", kStackBytes, nullptr, kPriority, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        s_paths.clear();
        s_running = false;
        return false;
    }
    return true;
}

FileOperationStatus file_operation_status() {
    std::lock_guard<std::mutex> lock(s_mutex);
    return s_status;
}

void file_operation_cancel() {
    s_cancel = true;
}
