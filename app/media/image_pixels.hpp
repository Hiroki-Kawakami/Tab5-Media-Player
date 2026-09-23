/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>

struct ImagePixels {
    uint8_t *data = nullptr;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t stride = 0;
    std::size_t bytes = 0;
    bool rgb888 = false;
    /* How `data` is freed; null means heap_caps_free(). */
    void (*release)(uint8_t *data) = nullptr;

    ~ImagePixels();
};
