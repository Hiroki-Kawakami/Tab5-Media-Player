/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>

constexpr uint8_t kImaAdpcmMaxChannels = 2;

std::size_t ima_adpcm_decode(const uint8_t *block, std::size_t len, uint8_t channels,
                             int16_t *out, std::size_t max_frames);
