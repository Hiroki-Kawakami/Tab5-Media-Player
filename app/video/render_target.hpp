/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include "bsp_types.h"

using VideoPresenterRelease = void (*)(void *ctx);

inline constexpr uint32_t kScaleDenominator = 16;

struct RenderTarget {
    void *framebuffer = nullptr;
    std::size_t framebuffer_bytes = 0;
    bsp_size_t panel = {};
    bsp_size_t source = {};
    bsp_rotation_t rotation = BSP_ROTATION_0;
    uint32_t scale_n = kScaleDenominator;
    bsp_rect_t rect = {};
};
