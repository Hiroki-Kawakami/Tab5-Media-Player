/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace airplay {

/* The body has to watch its own quit flag; join() only waits for it to return. */
class Task {
public:
    Task() = default;
    Task(const Task &) = delete;
    Task &operator=(const Task &) = delete;
    ~Task() { join(); }

    bool start(const char *name, uint32_t stack_bytes, UBaseType_t priority,
               std::function<void()> body);
    void join();

private:
    static void entry(void *arg);

    std::function<void()> body_;
    std::atomic<bool> running_{ false };
};

}  // namespace airplay
