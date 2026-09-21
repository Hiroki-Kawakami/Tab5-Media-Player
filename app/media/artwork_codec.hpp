/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/psram_allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

struct CoverPixels {
    uint8_t *data = nullptr;
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t stride = 0;
    std::size_t bytes = 0;
    bool rgb888 = false;

    ~CoverPixels();
};

std::shared_ptr<CoverPixels> artwork_decode(const uint8_t *data, std::size_t size, int32_t side,
                                            bool rgb888);
bool artwork_encode(const CoverPixels &pixels, PsramVector<uint8_t> *out);

void artwork_codec_idle();
void artwork_codec_close();
