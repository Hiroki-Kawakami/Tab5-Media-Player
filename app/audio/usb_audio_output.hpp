/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "esp_err.h"
#include "usb_host_uac.hpp"
#include <cstddef>
#include <cstdint>
#include <memory>

// Converts PCM to a format the device declares. A stream the device cannot take
// is consumed in real time and dropped.
class UsbAudioOutput {
public:
    explicit UsbAudioOutput(std::shared_ptr<usb_host::UacDevice> device);
    ~UsbAudioOutput();
    UsbAudioOutput(const UsbAudioOutput &) = delete;
    UsbAudioOutput &operator=(const UsbAudioOutput &) = delete;

    const std::shared_ptr<usb_host::UacDevice> &device() const { return device_; }
    void open(uint32_t rate, uint8_t bits, uint8_t channels);
    void close();
    esp_err_t write(const void *data, std::size_t len);

private:
    int choose_format() const;
    std::size_t convert(const uint8_t *in, std::size_t frames, uint8_t *out) const;

    std::shared_ptr<usb_host::UacDevice> device_;
    uint8_t *staging_ = nullptr;
    bool open_ = false;
    bool silent_ = false;
    bool passthrough_ = false;
    uint32_t rate_ = 0;
    uint8_t bits_ = 0;
    uint8_t channels_ = 0;
    uint8_t out_channels_ = 0;
    uint8_t out_bytes_ = 0;
};
