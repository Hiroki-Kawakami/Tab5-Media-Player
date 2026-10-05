/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include "bsp.h"

namespace usb_host {
class UacCaptureDevice;
class UvcDevice;
}

inline constexpr std::size_t kSharedSramBytes = 245760;
inline constexpr const char *kSdMountPoint = "/sdcard";
inline constexpr const char *kUsbMountPoint = "/usb";

struct SharedSram {
    void *base;
    std::size_t bytes;
    void *halves[2];
    std::size_t half_bytes;
};

inline bool path_is_under(const std::string &path, const std::string &mount_point) {
    if (path.compare(0, mount_point.size(), mount_point) != 0) return false;
    return path.size() == mount_point.size() || path[mount_point.size()] == '/';
}

void app_entry();

/* ESP_OK when already mounted; USB answers ESP_ERR_NOT_FOUND with no drive. */
esp_err_t media_player_mount_sd();
esp_err_t media_player_mount_usb();
bool media_player_usb_connected();
std::shared_ptr<usb_host::UvcDevice> media_player_camera();
std::shared_ptr<usb_host::UacCaptureDevice> media_player_capture_audio();

SharedSram media_player_acquire_sram();
void media_player_release_sram();

esp_err_t media_player_set_display_pixel_format(bsp_pixel_format_t format);
