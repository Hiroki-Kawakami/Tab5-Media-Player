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
    Bmp,
    Gif,
    Tiff,
    Webp,
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

/* What turns the stored picture upright, in the order of the EXIF orientations
   1-8. Rotations are clockwise. */
enum class ImageOrientation : uint8_t {
    None,
    MirrorX,
    Rot180,
    MirrorY,
    Transpose,
    Rot90,
    Transverse,
    Rot270,
};

ImageOrientation image_orientation_from_exif(uint8_t value);
bool image_orientation_swaps(ImageOrientation orientation);

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

/* `box` is the upright picture's; the notes keep the stored size. */
std::shared_ptr<ImagePixels> image_decode(const uint8_t *data, std::size_t size, ImageSize box,
                                          bool rgb888, ImageOrientation orientation,
                                          const volatile bool *cancel,
                                          ImageNotes *notes = nullptr);
/* Decodes `size` bytes at `offset` in the file; zero size is up to the end. */
std::shared_ptr<ImagePixels> image_decode_file(const std::string &path, int64_t offset,
                                               int64_t size, ImageSize box, bool rgb888,
                                               ImageOrientation orientation,
                                               const volatile bool *cancel,
                                               ImageNotes *notes = nullptr);
/* Decodes into `dst` as packed rows, blocking while another task holds the
   JPEG engine; `contended` is called first when it has to wait. */
bool image_decode_into(const uint8_t *data, std::size_t size, ImageSize box, bool rgb888,
                       uint8_t *dst, std::size_t capacity, ImageSize *out,
                       void (*contended)(void *ctx) = nullptr, void *ctx = nullptr);
/* Fits `box` exactly, enlarging too, as packed rows in `dst`. `scratch` takes
   the intermediate picture and must not overlap `dst`; both 64-byte aligned. */
bool image_decode_to_fit(const uint8_t *data, std::size_t size, ImageSize box, bool rgb888,
                         uint8_t *dst, std::size_t capacity, uint8_t *scratch,
                         std::size_t scratch_capacity, ImageSize *out);
struct PackedYuvImage;

std::shared_ptr<ImagePixels> image_from_packed_yuv(const PackedYuvImage &image, ImageSize box,
                                                   bool rgb888, ImageOrientation orientation);

/* `store` gets the encoded bytes, which live only for the call. */
bool image_encode(const ImagePixels &pixels,
                  const std::function<bool(const uint8_t *data, std::size_t size)> &store);

void image_codec_init();
void image_codec_close();
