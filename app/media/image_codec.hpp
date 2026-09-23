/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/image_pixels.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

enum class ImageFormat {
    Unknown,
    Jpeg,
    Png,
};

struct ImageSize {
    int16_t width = 0;
    int16_t height = 0;

    bool valid() const { return width > 0 && height > 0; }
    int32_t longest() const { return width > height ? width : height; }
    bool operator==(const ImageSize &other) const {
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

std::shared_ptr<ImagePixels> image_decode(const uint8_t *data, std::size_t size, ImageSize box,
                                          bool rgb888, const volatile bool *cancel,
                                          ImageNotes *notes = nullptr);
std::shared_ptr<ImagePixels> image_decode_file(const std::string &path, ImageSize box, bool rgb888,
                                               const volatile bool *cancel,
                                               ImageNotes *notes = nullptr);
/* Decodes into `dst` as packed rows, blocking while another task holds the
   JPEG engine; `contended` is called first when it has to wait. */
bool image_decode_into(const uint8_t *data, std::size_t size, ImageSize box, bool rgb888,
                       uint8_t *dst, std::size_t capacity, ImageSize *out,
                       void (*contended)(void *ctx) = nullptr, void *ctx = nullptr);
/* `store` gets the encoded bytes, which live only for the call. */
bool image_encode(const ImagePixels &pixels,
                  const std::function<bool(const uint8_t *data, std::size_t size)> &store);

void image_codec_init();
void image_codec_close();
