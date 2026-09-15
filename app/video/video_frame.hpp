/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>

struct FrameBuffer {
    uint8_t *data = nullptr;
    std::size_t capacity = 0;
};

class FrameAllocator {
public:
    virtual FrameBuffer lease(uint32_t pic_w, uint32_t pic_h) = 0;

protected:
    ~FrameAllocator() = default;
};

struct DecodedFrame {
    const uint8_t *pixels = nullptr;
    uint32_t pic_w = 0;
    uint32_t pic_h = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};
