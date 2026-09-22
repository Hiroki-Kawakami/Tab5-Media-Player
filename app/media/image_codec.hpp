/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/image_pixels.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

enum class ImageFormat {
    Unknown,
    Jpeg,
    Png,
};

struct ImageLoad {
    std::shared_ptr<ImagePixels> pixels;
    uint32_t source_width = 0;
    uint32_t source_height = 0;
    int64_t file_bytes = 0;
    ImageFormat format = ImageFormat::Unknown;
    bool hardware = false;
    bool cancelled = false;
    std::string error;
};

ImageLoad image_decode_file(const std::string &path, int32_t box_w, int32_t box_h, bool rgb888,
                            const volatile bool *cancel);

void image_codec_close();
