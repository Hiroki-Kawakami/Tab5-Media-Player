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

    bool empty() const;
};

/* `data` is the head of a JPEG or PNG file, which may stop inside the segment
   that carries the tags: whatever is readable is taken. */
bool image_exif_parse(const uint8_t *data, std::size_t size, ImageExif *out);
