/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "usb_audio_output.hpp"
#include "audf_gain.h"
#include "audf_mixer.h"
#include "audf_resampler.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <algorithm>
#include <utility>
#include <vector>

static const char *TAG = "usb_audio";

static constexpr std::size_t kChunkFrames = 512;
static constexpr uint32_t kAllocCaps = MALLOC_CAP_SPIRAM;
static constexpr uint32_t kGainFadeMs = 100;
static constexpr uint32_t kMaxRateFactor = 8;

UsbAudioOutput::UsbAudioOutput(std::shared_ptr<usb_host::UacDevice> device,
                               const std::atomic<float> *gain)
    : device_(std::move(device)), gain_target_(gain) {}

UsbAudioOutput::~UsbAudioOutput() {
    close();
}

static bool channels_convertible(uint8_t in, uint8_t out) {
    return in == out || in == 1 || out == 1;
}

static bool integer_ratio(uint32_t a, uint32_t b) {
    return a % b == 0 || b % a == 0;
}

int UsbAudioOutput::choose_format(uint32_t rate) const {
    const auto &formats = device_->formats();
    int best = -1;
    int best_score = 0;
    for (std::size_t i = 0; i < formats.size(); i++) {
        const usb_host::UacFormat &format = formats[i];
        if (format.subframe_bytes < 2 || !format.supports(rate)) continue;
        if (!channels_convertible(channels_, format.channels)) continue;
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

UsbAudioOutput::Choice UsbAudioOutput::choose() const {
    Choice choice;
    choice.format = choose_format(rate_);
    if (choice.format >= 0) {
        choice.rate = rate_;
        return choice;
    }

    std::vector<uint32_t> rates;
    for (const usb_host::UacFormat &format : device_->formats()) {
        if (format.rates.empty()) {
            rates.push_back(format.min_rate);
            rates.push_back(format.max_rate);
            for (uint32_t factor = 2; factor <= kMaxRateFactor; factor++) {
                if (format.supports(rate_ * factor)) rates.push_back(rate_ * factor);
                if (rate_ % factor == 0 && format.supports(rate_ / factor)) {
                    rates.push_back(rate_ / factor);
                }
            }
        } else {
            rates.insert(rates.end(), format.rates.begin(), format.rates.end());
        }
    }
    auto better = [this](uint32_t a, uint32_t b) {
        const bool a_integer = integer_ratio(a, rate_);
        if (a_integer != integer_ratio(b, rate_)) return a_integer;
        if ((a >= rate_) != (b >= rate_)) return a >= rate_;
        return a >= rate_ ? a < b : a > b;
    };
    std::sort(rates.begin(), rates.end(), better);
    for (uint32_t rate : rates) {
        const int format = choose_format(rate);
        if (format < 0) continue;
        choice.format = format;
        choice.rate = rate;
        return choice;
    }
    return choice;
}

esp_err_t UsbAudioOutput::build(const Choice &choice, AudioContent content) {
    const uint8_t resample_channels = std::min(channels_, out_channels_);
    const bool mix = channels_ != out_channels_;
    const bool resample = choice.rate != rate_;
    const bool gain = !device_->has_volume();

    audf_graph_config_t graph_config = {};
    graph_config.max_frames = kChunkFrames;
    graph_config.alloc_caps = kAllocCaps;
    esp_err_t err = audf_graph_create(&graph_config, &graph_);
    if (err != ESP_OK) return err;

    if (mix) {
        audf_mixer_config_t config = {};
        config.fmt = AUDF_FMT_S32;
        config.out_channels = out_channels_;
        config.num_inputs = 1;
        config.in_channels = &channels_;
        config.alloc_caps = kAllocCaps;
        err = audf_mixer_create(&config, &mixer_);
        if (err != ESP_OK) return err;
    }
    if (resample) {
        audf_resampler_config_t config = {};
        if (integer_ratio(choice.rate, rate_)) {
            config.kind = AUDF_RESAMPLER_INTEGER;
        } else {
            config.kind = content == AudioContent::Music ? AUDF_RESAMPLER_POLYPHASE
                                                         : AUDF_RESAMPLER_CUBIC;
        }
        config.fmt = AUDF_FMT_S32;
        config.channels = resample_channels;
        config.in_rate = rate_;
        config.out_rate = choice.rate;
        config.max_in_frames = kChunkFrames;
        config.alloc_caps = kAllocCaps;
        err = audf_resampler_create(&config, &resampler_);
        if (err != ESP_OK) return err;
        ESP_LOGI(TAG, "resample %u -> %u Hz (%s)", (unsigned)rate_, (unsigned)choice.rate,
                 config.kind == AUDF_RESAMPLER_INTEGER    ? "integer"
                 : config.kind == AUDF_RESAMPLER_POLYPHASE ? "polyphase"
                                                           : "cubic");
    }
    if (gain) {
        audf_gain_config_t config = {};
        config.fmt = AUDF_FMT_S32;
        config.channels = out_channels_;
        config.sample_rate = choice.rate;
        config.gain = gain_target_->load();
        config.alloc_caps = kAllocCaps;
        err = audf_gain_create(&config, &gain_);
        if (err != ESP_OK) return err;
        gain_applied_ = config.gain;
    }

    audf_node_t *node = input_ = audf_graph_add_input(graph_, AUDF_FMT_S32, channels_);
    const bool resample_first = channels_ < out_channels_;
    if (resample && resample_first) node = audf_graph_add_resampler(graph_, node, resampler_, nullptr);
    if (mix) node = audf_graph_add_mixer(graph_, &node, mixer_);
    if (resample && !resample_first) node = audf_graph_add_resampler(graph_, node, resampler_, nullptr);
    if (gain) node = audf_graph_add_gain(graph_, node, gain_);
    audf_graph_add_sink(graph_, node, sink, this);
    err = audf_graph_build(graph_);
    if (err != ESP_OK) return err;

    staging_ = (int32_t *)heap_caps_malloc(kChunkFrames * channels_ * sizeof(int32_t), kAllocCaps);
    if (!staging_) staging_ = (int32_t *)heap_caps_malloc(kChunkFrames * channels_ * sizeof(int32_t),
                                                          MALLOC_CAP_DEFAULT);
    return staging_ ? ESP_OK : ESP_ERR_NO_MEM;
}

void UsbAudioOutput::destroy() {
    audf_graph_destroy(graph_);
    audf_mixer_destroy(mixer_);
    audf_resampler_destroy(resampler_);
    audf_gain_destroy(gain_);
    heap_caps_free(staging_);
    graph_ = nullptr;
    input_ = nullptr;
    mixer_ = nullptr;
    resampler_ = nullptr;
    gain_ = nullptr;
    staging_ = nullptr;
}

void UsbAudioOutput::open(uint32_t rate, uint8_t bits, uint8_t channels, AudioContent content) {
    close();
    rate_ = rate;
    bits_ = bits;
    channels_ = channels;
    open_ = true;
    silent_ = true;

    const Choice choice = choose();
    if (choice.format < 0) {
        ESP_LOGE(TAG, "no device format for %u Hz %u bit x%u", (unsigned)rate, (unsigned)bits,
                 (unsigned)channels);
        return;
    }
    const usb_host::UacFormat &format = device_->formats()[choice.format];
    out_channels_ = format.channels;
    out_bytes_ = format.subframe_bytes;
    out_pcm_ = out_bytes_ == 2 ? AUDF_PCM_S16 : out_bytes_ == 3 ? AUDF_PCM_S24 : AUDF_PCM_S32;
    passthrough_ = choice.rate == rate && out_channels_ == channels && out_bytes_ * 8 == bits &&
                   bits != 8 && device_->has_volume();
    if (!passthrough_) {
        const esp_err_t err = build(choice, content);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "processing for %u Hz x%u -> %u Hz x%u: %s", (unsigned)rate,
                     (unsigned)channels, (unsigned)choice.rate, (unsigned)out_channels_,
                     esp_err_to_name(err));
            destroy();
            return;
        }
    }
    const esp_err_t err = device_->open((std::size_t)choice.format, choice.rate);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "open %u Hz: %s", (unsigned)choice.rate, esp_err_to_name(err));
        destroy();
        return;
    }
    silent_ = false;
    ESP_LOGI(TAG, "%u Hz %u bit x%u -> %u Hz %u bit x%u", (unsigned)rate, (unsigned)bits,
             (unsigned)channels, (unsigned)choice.rate, (unsigned)format.bit_resolution,
             (unsigned)out_channels_);
}

void UsbAudioOutput::close() {
    if (!open_) return;
    open_ = false;
    if (!silent_) {
        device_->drain();
        device_->close();
    }
    destroy();
}

void UsbAudioOutput::to_s32(const uint8_t *in, std::size_t frames, int32_t *out) const {
    const std::size_t samples = frames * channels_;
    switch (bits_) {
    case 8:
        for (std::size_t i = 0; i < samples; i++) out[i] = (int32_t)(int8_t)(in[i] ^ 0x80) * 65536;
        return;
    case 24: audf_convert_from_pcm(in, AUDF_PCM_S24, out, AUDF_FMT_S32, samples); return;
    case 32: audf_convert_from_pcm(in, AUDF_PCM_S32, out, AUDF_FMT_S32, samples); return;
    default: audf_convert_from_pcm(in, AUDF_PCM_S16, out, AUDF_FMT_S32, samples); return;
    }
}

esp_err_t UsbAudioOutput::sink(void *user, void *data, std::size_t frames) {
    auto *self = static_cast<UsbAudioOutput *>(user);
    const std::size_t samples = frames * self->out_channels_;
    audf_convert_to_pcm(data, AUDF_FMT_S32, data, self->out_pcm_, samples);
    return self->device_->write(data, samples * self->out_bytes_);
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

    if (gain_) {
        const float target = gain_target_->load();
        if (target != gain_applied_) {
            audf_gain_set(gain_, target, kGainFadeMs);
            gain_applied_ = target;
        }
    }
    const uint8_t *in = static_cast<const uint8_t *>(data);
    std::size_t frames = len / in_frame;
    while (frames > 0) {
        const std::size_t count = std::min(frames, kChunkFrames);
        to_s32(in, count, staging_);
        const esp_err_t err = audf_graph_write(graph_, input_, staging_, count);
        if (err != ESP_OK) return err;
        in += count * in_frame;
        frames -= count;
    }
    return ESP_OK;
}
