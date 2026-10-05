/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "audf_convert.h"
#include "audf_graph.h"
#include "audio_output.hpp"
#include "esp_err.h"
#include "usb_host_uac.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

// Converts PCM to a format and rate the device declares. A stream that cannot
// be set up is consumed in real time and dropped.
class UsbAudioOutput {
public:
    // `gain` is applied only when the device has no volume control.
    UsbAudioOutput(std::shared_ptr<usb_host::UacDevice> device, const std::atomic<float> *gain);
    ~UsbAudioOutput();
    UsbAudioOutput(const UsbAudioOutput &) = delete;
    UsbAudioOutput &operator=(const UsbAudioOutput &) = delete;

    const std::shared_ptr<usb_host::UacDevice> &device() const { return device_; }
    void open(uint32_t rate, uint8_t bits, uint8_t channels, AudioContent content);
    void close();
    esp_err_t write(const void *data, std::size_t len);

private:
    struct Choice {
        int format = -1;
        uint32_t rate = 0;
    };

    Choice choose() const;
    int choose_format(uint32_t rate) const;
    esp_err_t build(const Choice &choice, AudioContent content);
    void destroy();
    void to_s32(const uint8_t *in, std::size_t frames, int32_t *out) const;
    static esp_err_t sink(void *user, void *data, std::size_t frames);

    std::shared_ptr<usb_host::UacDevice> device_;
    const std::atomic<float> *gain_target_;
    float gain_applied_ = -1.0f;
    bool open_ = false;
    bool silent_ = false;
    bool passthrough_ = false;
    uint32_t rate_ = 0;
    uint8_t bits_ = 0;
    uint8_t channels_ = 0;
    uint8_t out_channels_ = 0;
    uint8_t out_bytes_ = 0;
    audf_pcm_t out_pcm_ = AUDF_PCM_S16;

    audf_graph_t *graph_ = nullptr;
    audf_node_t *input_ = nullptr;
    audf_mixer_t *mixer_ = nullptr;
    audf_resampler_t *resampler_ = nullptr;
    audf_gain_t *gain_ = nullptr;
    int32_t *staging_ = nullptr;
};
