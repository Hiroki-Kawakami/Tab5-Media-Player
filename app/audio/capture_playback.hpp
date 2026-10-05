/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "usb_host_uac.hpp"
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

// Plays a USB capture device through audio_output, keeping up with the
// device's clock by dropping or repeating single frames.
class CapturePlayback {
public:
    CapturePlayback() = default;
    ~CapturePlayback();
    CapturePlayback(const CapturePlayback &) = delete;
    CapturePlayback &operator=(const CapturePlayback &) = delete;

    bool start(std::shared_ptr<usb_host::UacCaptureDevice> device, std::string *error);
    void stop();

private:
    static void main(void *arg);
    void run();
    bool prefill();

    std::shared_ptr<usb_host::UacCaptureDevice> device_;
    uint8_t *buffer_ = nullptr;
    std::size_t chunk_bytes_ = 0;
    std::size_t frame_bytes_ = 0;
    std::size_t target_bytes_ = 0;
    std::size_t tolerance_bytes_ = 0;
    SemaphoreHandle_t stopped_ = nullptr;
    std::atomic<bool> quit_{ false };
};
