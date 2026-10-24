/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>

struct ImageExif {
    char make[32] = {};
    char model[40] = {};
    char lens[48] = {};
    char software[32] = {};
    char taken[24] = {};
    uint32_t iso = 0;
    uint32_t shutter_num = 0;
    uint32_t shutter_den = 0;
    float aperture = 0;
    float focal_mm = 0;
    float exposure_bias = 0;
    uint16_t focal35_mm = 0;
    uint16_t flash = 0;
    uint8_t orientation = 0;
    bool has_flash = false;
    bool has_bias = false;
    /* The JPEG a camera leaves in IFD1, as an offset into the file, so a small
       box can be served without reading or decoding the picture itself. Zero
       when there is none. */
    uint32_t thumb_at = 0;
    uint32_t thumb_bytes = 0;
    uint16_t thumb_width = 0;
    uint16_t thumb_height = 0;

    bool empty() const;
};

/* `data` is the head of a JPEG, PNG, TIFF or WebP file, which may stop inside
   the segment that carries the tags: whatever is readable is taken. */
bool image_exif_parse(const uint8_t *data, std::size_t size, ImageExif *out);

/* For the head of a WebP file that says it carries EXIF but does not reach the
   chunk: the offset of the first chunk header past the head. Zero otherwise. */
std::size_t image_exif_webp_resume(const uint8_t *data, std::size_t size);

/* `data` is the payload of a WebP EXIF chunk, which starts `at` bytes into the
   file. */
bool image_exif_parse_webp_chunk(const uint8_t *data, std::size_t size, std::size_t at,
                                 ImageExif *out);
