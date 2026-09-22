/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/image_pixels.hpp"
#include "media/psram_allocator.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

std::shared_ptr<ImagePixels> artwork_decode(const uint8_t *data, std::size_t size, int32_t side,
                                            bool rgb888);
bool artwork_encode(const ImagePixels &pixels, PsramVector<uint8_t> *out);

void artwork_codec_close();
