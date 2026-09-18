/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include "bsp.h"

inline constexpr std::size_t kSharedSramBytes = 245760;
inline constexpr const char *kSdMountPoint = "/sdcard";
inline constexpr const char *kUsbMountPoint = "/usb";

struct SharedSram {
    void *base;
    std::size_t bytes;
    void *halves[2];
    std::size_t half_bytes;
};

void app_entry();

SharedSram media_player_acquire_sram();
void media_player_release_sram();
