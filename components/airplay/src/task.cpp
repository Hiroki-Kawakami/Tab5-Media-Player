/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "task.hpp"
#include "esp_heap_caps.h"

namespace airplay {

void Task::entry(void *arg) {
    auto *self = static_cast<Task *>(arg);
    self->body_();
    self->running_ = false;
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

bool Task::start(const char *name, uint32_t stack_bytes, UBaseType_t priority,
                 std::function<void()> body) {
    join();
    body_ = std::move(body);
    running_ = true;
#ifdef ESP_PLATFORM
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        entry, name, stack_bytes, this, priority, nullptr, tskNO_AFFINITY,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    const BaseType_t created = xTaskCreate(entry, name, stack_bytes, this, priority, nullptr);
#endif
    if (created != pdPASS) running_ = false;
    return created == pdPASS;
}

void Task::join() {
    while (running_) vTaskDelay(1);
    body_ = nullptr;
}

}  // namespace airplay
