/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "capture_playback.hpp"
#include "audio_output.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <cstring>

static const char *TAG = "capture_playback";

static constexpr uint32_t kPreferredRate = 48000;
static constexpr uint32_t kChunkMs = 5;
static constexpr uint32_t kTargetMs = 20;
static constexpr uint32_t kToleranceMs = 3;
static constexpr uint32_t kReadTimeoutMs = 100;
static constexpr uint32_t kPrefillPollMs = 2;
static constexpr float kFillSmoothing = 1.0f / 64.0f;
static constexpr uint32_t kStackBytes = 4096;
static constexpr UBaseType_t kPriority = 6;

struct Choice {
    int format = -1;
    uint32_t rate = 0;
};

static uint32_t pick_rate(const usb_host::UacFormat &format) {
    if (format.supports(kPreferredRate)) return kPreferredRate;
    if (!format.rates.empty()) return format.rates.back();
    return format.max_rate;
}

static Choice choose(const std::vector<usb_host::UacFormat> &formats) {
    Choice choice;
    for (std::size_t i = 0; i < formats.size(); i++) {
        const usb_host::UacFormat &format = formats[i];
        if (format.subframe_bytes != 2 || format.channels < 1 || format.channels > 2) continue;
        const uint32_t rate = pick_rate(format);
        if (!rate) continue;
        const bool better = choice.format < 0 ||
                            format.channels > formats[choice.format].channels ||
                            (rate == kPreferredRate && choice.rate != kPreferredRate);
        if (better) choice = { (int)i, rate };
    }
    return choice;
}

CapturePlayback::~CapturePlayback() {
    stop();
}

bool CapturePlayback::start(std::shared_ptr<usb_host::UacCaptureDevice> device,
                            std::string *error) {
    stop();
    const Choice choice = choose(device->formats());
    if (choice.format < 0) {
        *error = "no 16-bit audio from the capture device";
        return false;
    }
    const uint8_t channels = device->formats()[choice.format].channels;
    frame_bytes_ = channels * sizeof(int16_t);
    const std::size_t frames_per_ms = (choice.rate + 999) / 1000;
    chunk_bytes_ = frames_per_ms * kChunkMs * frame_bytes_;
    target_bytes_ = frames_per_ms * kTargetMs * frame_bytes_;
    tolerance_bytes_ = frames_per_ms * kToleranceMs * frame_bytes_;
    buffer_ = static_cast<uint8_t *>(
        heap_caps_malloc(chunk_bytes_ + frame_bytes_, MALLOC_CAP_SPIRAM));
    if (!buffer_) {
        *error = "no memory for capture audio";
        return false;
    }
    esp_err_t err = device->open(choice.format, choice.rate);
    if (err == ESP_OK) err = audio_output_open(choice.rate, 16, channels, AudioContent::Video);
    if (err != ESP_OK) {
        device->close();
        heap_caps_free(buffer_);
        buffer_ = nullptr;
        *error = std::string("capture audio did not start: ") + esp_err_to_name(err);
        return false;
    }
    ESP_LOGI(TAG, "%u Hz, %u ch", (unsigned)choice.rate, channels);

    device_ = std::move(device);
    stopped_ = xSemaphoreCreateBinary();
    quit_ = false;
#ifdef ESP_PLATFORM
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        main, "capture_audio", kStackBytes, this, kPriority, nullptr, 1, MALLOC_CAP_SPIRAM);
#else
    const BaseType_t created =
        xTaskCreatePinnedToCore(main, "capture_audio", kStackBytes, this, kPriority, nullptr, 1);
#endif
    if (stopped_ && created == pdPASS) return true;
    if (stopped_) vSemaphoreDelete(stopped_);
    stopped_ = nullptr;
    stop();
    *error = "no memory for the capture audio task";
    return false;
}

void CapturePlayback::stop() {
    quit_ = true;
    if (stopped_) {
        xSemaphoreTake(stopped_, portMAX_DELAY);
        vSemaphoreDelete(stopped_);
        stopped_ = nullptr;
    }
    if (device_) {
        device_->close();
        device_.reset();
        audio_output_close();
    }
    heap_caps_free(buffer_);
    buffer_ = nullptr;
}

void CapturePlayback::main(void *arg) {
    auto *self = static_cast<CapturePlayback *>(arg);
    self->run();
    xSemaphoreGive(self->stopped_);
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

bool CapturePlayback::prefill() {
    while (!quit_ && device_->connected()) {
        if (device_->available() >= target_bytes_) return true;
        vTaskDelay(pdMS_TO_TICKS(kPrefillPollMs));
    }
    return false;
}

void CapturePlayback::run() {
    if (!prefill()) return;
    float fill = (float)target_bytes_;
    while (!quit_) {
        std::size_t got = 0;
        const esp_err_t err = device_->read(buffer_, chunk_bytes_, &got, kReadTimeoutMs);
        if (err == ESP_ERR_TIMEOUT) {
            if (!prefill()) return;
            fill = (float)target_bytes_;
            continue;
        }
        if (err != ESP_OK) return;

        fill += ((float)device_->available() - fill) * kFillSmoothing;
        if (fill > (float)(target_bytes_ + tolerance_bytes_) && got > frame_bytes_) {
            got -= frame_bytes_;
        } else if (fill < (float)(target_bytes_ - tolerance_bytes_) && got >= frame_bytes_) {
            memcpy(buffer_ + got, buffer_ + got - frame_bytes_, frame_bytes_);
            got += frame_bytes_;
        }
        audio_output_write(buffer_, got);
    }
}
