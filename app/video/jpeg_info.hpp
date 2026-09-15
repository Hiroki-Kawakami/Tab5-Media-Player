/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

bool jpeg_image_size(const uint8_t *data, std::size_t len,
                     uint32_t *width, uint32_t *height, std::string *error);
