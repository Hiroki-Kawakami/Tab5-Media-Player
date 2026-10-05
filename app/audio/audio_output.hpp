/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "esp_err.h"
#include "lvgl.h"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace usb_host {
class UacDevice;
}

enum class AudioRoute { Speaker, Headphone, Usb };
enum class AudioContent { Music, Video };

// After bsp_init() and settings_init().
void audio_output_init();

AudioRoute audio_output_route();
const char *audio_route_name(AudioRoute route);

int audio_output_volume();
void audio_output_set_volume(int percent);
// Not stored; lasts until the route changes or the volume is set.
void audio_output_apply_volume(int percent);
// `on_change` runs on the LVGL thread, also when the route changes.
void audio_output_volume_observe(lv_obj_t *owner, std::function<void(int)> on_change);

void audio_output_set_mute(bool mute);
bool audio_output_get_mute();

void audio_output_usb_connected(std::shared_ptr<usb_host::UacDevice> device);
void audio_output_usb_disconnected(const std::shared_ptr<usb_host::UacDevice> &device);

// Opening a running stream with another format reopens it.
esp_err_t audio_output_open(uint32_t rate, uint8_t bits, uint8_t channels, AudioContent content);
void audio_output_close();
// May filter `data` in place. Blocks while the output is full.
esp_err_t audio_output_write(void *data, std::size_t len);
