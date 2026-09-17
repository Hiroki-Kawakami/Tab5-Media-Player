/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_threads.hpp"
#include "freertos/semphr.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

static constexpr uint32_t kWorkerStackBytes = 6144;
static constexpr UBaseType_t kWorkerPriority = 2;
static constexpr BaseType_t kWorkerCore = 1;

struct Spawn {
    void (*entry)(void *);
    void *arg;
};

static void *sem_create(void *, uint32_t max, uint32_t initial) {
    return xSemaphoreCreateCounting(max, initial);
}

static void sem_delete(void *, void *sem) {
    vSemaphoreDelete(static_cast<SemaphoreHandle_t>(sem));
}

static void sem_take(void *, void *sem) {
    xSemaphoreTake(static_cast<SemaphoreHandle_t>(sem), portMAX_DELAY);
}

static void sem_give(void *, void *sem) {
    xSemaphoreGive(static_cast<SemaphoreHandle_t>(sem));
}

static void worker_main(void *arg) {
    Spawn spawn = *static_cast<Spawn *>(arg);
    delete static_cast<Spawn *>(arg);
    spawn.entry(spawn.arg);
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

static bool spawn(void *, void (*entry)(void *), void *arg) {
    auto *s = new Spawn{ entry, arg };
    if (h264_create_task(worker_main, "h264_post", kWorkerStackBytes, s, kWorkerPriority,
                         kWorkerCore, nullptr) != pdPASS) {
        delete s;
        return false;
    }
    return true;
}

static constexpr h264_dec_threads_t kThreads = {
    sem_create, sem_delete, sem_take, sem_give, spawn, nullptr,
};

const h264_dec_threads_t *h264_threads() {
    return &kThreads;
}

BaseType_t h264_create_task(TaskFunction_t entry, const char *name, uint32_t stack_bytes,
                            void *arg, UBaseType_t priority, BaseType_t core,
                            TaskHandle_t *handle) {
#ifdef ESP_PLATFORM
    return xTaskCreatePinnedToCoreWithCaps(entry, name, stack_bytes, arg, priority, handle, core,
                                           MALLOC_CAP_SIMD);
#else
    return xTaskCreatePinnedToCore(entry, name, stack_bytes, arg, priority, handle, core);
#endif
}
