/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "jpeg_info.hpp"

bool jpeg_image_size(const uint8_t *data, std::size_t len,
                     uint32_t *width, uint32_t *height, std::string *error) {
    if (len < 4 || data[0] != 0xFF || data[1] != 0xD8) {
        *error = "not a JPEG file";
        return false;
    }
    std::size_t i = 2;
    while (i + 4 <= len) {
        if (data[i] != 0xFF) {
            *error = "damaged JPEG header";
            return false;
        }
        const uint8_t marker = data[i + 1];
        if (marker == 0xFF) {
            i++;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
            i += 2;
            continue;
        }
        const std::size_t segment = ((std::size_t)data[i + 2] << 8) | data[i + 3];
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 && marker != 0xCC) {
            if (marker != 0xC0 && marker != 0xC1) {
                *error = "unsupported JPEG (not baseline)";
                return false;
            }
            if (i + 9 > len) break;
            *height = ((uint32_t)data[i + 5] << 8) | data[i + 6];
            *width = ((uint32_t)data[i + 7] << 8) | data[i + 8];
            return *width && *height;
        }
        if (marker == 0xDA) break;
        i += 2 + segment;
    }
    *error = "no JPEG frame header";
    return false;
}
