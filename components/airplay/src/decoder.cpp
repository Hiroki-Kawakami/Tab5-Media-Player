/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "decoder.hpp"

#include "audf_alac.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

namespace airplay {

static const char *TAG = "airplay";

static uint32_t decode_pcm(const uint8_t *data, std::size_t len, uint8_t channels, int16_t *out,
                           uint32_t max_frames) {
    uint32_t frames = (uint32_t)(len / (2u * channels));
    if (frames > max_frames) frames = max_frames;
    for (uint32_t i = 0; i < frames * channels; i++) {
        out[i] = (int16_t)((data[2 * i] << 8) | data[2 * i + 1]);
    }
    return frames;
}

Decoder::~Decoder() { audf_decoder_destroy(alac_); }

bool Decoder::open(const Format &format) {
    format_ = format;
    if (format.codec != Codec::Alac) return true;
    audf_alac_config_t config = {};
    config.cookie = format_.alac_cookie;
    config.cookie_len = sizeof(format_.alac_cookie);
    config.alloc_caps = MALLOC_CAP_SPIRAM;
    const esp_err_t err = audf_alac_decoder_create(&config, &alac_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ALAC decoder: %s", esp_err_to_name(err));
        return false;
    }
    return audf_decoder_fmt(alac_) == AUDF_FMT_S16 && audf_decoder_channels(alac_) == format.channels;
}

uint32_t Decoder::decode(const uint8_t *data, std::size_t len, int16_t *out,
                         uint32_t max_frames) {
    if (format_.codec == Codec::Pcm) {
        return decode_pcm(data, len, format_.channels, out, max_frames);
    }
    if (!alac_ || audf_decoder_max_frames(alac_) > max_frames) return 0;
    std::size_t frames = 0;
    const esp_err_t err = audf_decoder_decode(alac_, data, len, out, &frames);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ALAC decode: %s", esp_err_to_name(err));
        return 0;
    }
    return (uint32_t)frames;
}

}  // namespace airplay
