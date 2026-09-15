/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "video_frame.hpp"
#include "bsp_types.h"
#include "jpeg_decode_enhanced.h"
#include <string>

class MjpegDecoder {
public:
    bool open(bsp_pixel_format_t format, std::string *error);
    bool decode(const uint8_t *data, std::size_t len, FrameAllocator &allocator,
                DecodedFrame *out, std::string *error);

private:
    jpeg_enh_strip_decoder_handle_t decoder_ = nullptr;
    bsp_pixel_format_t format_ = BSP_PIXEL_FORMAT_RGB565;
};
