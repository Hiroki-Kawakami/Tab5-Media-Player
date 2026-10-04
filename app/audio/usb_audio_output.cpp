/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "usb_audio_output.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <utility>

static const char *TAG = "usb_audio";

static constexpr std::size_t kStagingBytes = 4096;

UsbAudioOutput::UsbAudioOutput(std::shared_ptr<usb_host::UacDevice> device)
    : device_(std::move(device)) {}

UsbAudioOutput::~UsbAudioOutput() {
    close();
    heap_caps_free(staging_);
}

static bool channels_convertible(uint8_t in, uint8_t out) {
    return in == out || (in == 1 && out == 2) || (in == 2 && out == 1);
}

int UsbAudioOutput::choose_format() const {
    const auto &formats = device_->formats();
    int best = -1;
    int best_score = 0;
    for (std::size_t i = 0; i < formats.size(); i++) {
        const usb_host::UacFormat &format = formats[i];
        if (!format.supports(rate_) || !channels_convertible(channels_, format.channels)) continue;
        const int depth = format.bit_resolution - bits_;
        int score = format.channels == channels_ ? 1000 : 0;
        score += depth >= 0 ? 500 - depth : 100 + depth;
        if (best < 0 || score > best_score) {
            best = (int)i;
            best_score = score;
        }
    }
    return best;
}

void UsbAudioOutput::open(uint32_t rate, uint8_t bits, uint8_t channels) {
    close();
    rate_ = rate;
    bits_ = bits;
    channels_ = channels;
    open_ = true;
    silent_ = true;

    const int format = choose_format();
    if (format < 0) {
        ESP_LOGE(TAG, "no device format for %u Hz %u bit x%u", (unsigned)rate, (unsigned)bits,
                 (unsigned)channels);
        return;
    }
    if (!staging_) {
        staging_ = (uint8_t *)heap_caps_malloc(kStagingBytes, MALLOC_CAP_SPIRAM);
        if (!staging_) staging_ = (uint8_t *)heap_caps_malloc(kStagingBytes, MALLOC_CAP_DEFAULT);
        if (!staging_) {
            ESP_LOGE(TAG, "out of memory for the staging buffer");
            return;
        }
    }
    const esp_err_t err = device_->open((std::size_t)format, rate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %u Hz: %s", (unsigned)rate, esp_err_to_name(err));
        return;
    }
    const usb_host::UacFormat &chosen = device_->formats()[format];
    out_channels_ = chosen.channels;
    out_bytes_ = chosen.subframe_bytes;
    passthrough_ = out_channels_ == channels_ && out_bytes_ * 8 == bits_ && bits_ != 8;
    silent_ = false;
    ESP_LOGI(TAG, "%u Hz %u bit x%u -> %u bit x%u", (unsigned)rate, (unsigned)bits,
             (unsigned)channels, (unsigned)chosen.bit_resolution, (unsigned)out_channels_);
}

void UsbAudioOutput::close() {
    if (!open_) return;
    open_ = false;
    if (silent_) return;
    device_->drain();
    device_->close();
}

static int32_t read_sample(const uint8_t *in, uint8_t bits) {
    switch (bits) {
    case 8: return (int32_t)((uint32_t)(in[0] ^ 0x80) << 24);
    case 16: return (int32_t)(((uint32_t)in[0] << 16) | ((uint32_t)in[1] << 24));
    case 24: return (int32_t)(((uint32_t)in[0] << 8) | ((uint32_t)in[1] << 16) | ((uint32_t)in[2] << 24));
    default: return (int32_t)((uint32_t)in[0] | ((uint32_t)in[1] << 8) | ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24));
    }
}

static void write_sample(uint8_t *out, int32_t value, uint8_t bytes) {
    for (uint8_t i = 0; i < bytes; i++) out[i] = (uint8_t)((uint32_t)value >> (32 - 8 * (bytes - i)));
}

std::size_t UsbAudioOutput::convert(const uint8_t *in, std::size_t frames, uint8_t *out) const {
    const std::size_t in_sample = bits_ / 8;
    for (std::size_t frame = 0; frame < frames; frame++) {
        if (channels_ == 2 && out_channels_ == 1) {
            const int32_t left = read_sample(in, bits_);
            const int32_t right = read_sample(in + in_sample, bits_);
            write_sample(out, (left >> 1) + (right >> 1), out_bytes_);
        } else {
            for (uint8_t channel = 0; channel < out_channels_; channel++) {
                const uint8_t source = channels_ == 1 ? 0 : channel;
                write_sample(out + channel * out_bytes_, read_sample(in + source * in_sample, bits_),
                             out_bytes_);
            }
        }
        in += in_sample * channels_;
        out += (std::size_t)out_bytes_ * out_channels_;
    }
    return frames * out_bytes_ * out_channels_;
}

esp_err_t UsbAudioOutput::write(const void *data, std::size_t len) {
    if (!open_) return ESP_ERR_INVALID_STATE;
    const std::size_t in_frame = (std::size_t)channels_ * (bits_ / 8);
    if (!in_frame) return ESP_ERR_INVALID_STATE;
    if (silent_) {
        vTaskDelay(pdMS_TO_TICKS((uint64_t)(len / in_frame) * 1000 / rate_));
        return ESP_OK;
    }
    if (passthrough_) return device_->write(data, len);

    const uint8_t *in = static_cast<const uint8_t *>(data);
    std::size_t frames = len / in_frame;
    const std::size_t chunk = kStagingBytes / ((std::size_t)out_bytes_ * out_channels_);
    while (frames > 0) {
        const std::size_t count = std::min(frames, chunk);
        const esp_err_t err = device_->write(staging_, convert(in, count, staging_));
        if (err != ESP_OK) return err;
        in += count * in_frame;
        frames -= count;
    }
    return ESP_OK;
}
