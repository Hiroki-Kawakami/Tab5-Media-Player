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
#include <string>

enum class ImageFormat {
    Unknown,
    Jpeg,
    Png,
};

struct ImageBox {
    int16_t width = 0;
    int16_t height = 0;

    bool valid() const { return width > 0 && height > 0; }
    int32_t longest() const { return width > height ? width : height; }
    bool operator==(const ImageBox &other) const {
        return width == other.width && height == other.height;
    }
};

struct ImageHeader {
    ImageFormat format = ImageFormat::Unknown;
    uint32_t width = 0;
    uint32_t height = 0;
    bool hardware = false;
};

/* What a decode learned about the picture, whether or not it produced pixels. */
struct ImageNotes {
    ImageHeader header;
    bool out_of_memory = false;
};

bool image_header(const uint8_t *data, std::size_t size, ImageHeader *out);

std::shared_ptr<ImagePixels> image_decode(const uint8_t *data, std::size_t size, ImageBox box,
                                          bool rgb888, const volatile bool *cancel,
                                          ImageNotes *notes = nullptr);
std::shared_ptr<ImagePixels> image_decode_file(const std::string &path, ImageBox box, bool rgb888,
                                               const volatile bool *cancel,
                                               ImageNotes *notes = nullptr);
bool image_encode(const ImagePixels &pixels, PsramVector<uint8_t> *out);

/* Rescales decoded pixels to fit `box` with PPA, for a picture that has to be
   shown at a new size before it can be decoded at that size. */
std::shared_ptr<ImagePixels> image_scale(const ImagePixels &src, ImageBox box);

void image_codec_close();
