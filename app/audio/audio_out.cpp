/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "audio_out.hpp"
#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>

#ifdef ESP_PLATFORM
extern "C" {
#include "esp_audio_dec.h"
#include "esp_mp3_dec.h"
}
#else
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}
#endif

static const char *TAG = "audio_out";

static constexpr std::size_t kPcmBytes = 64 * 1024;
static constexpr int kDefaultVolume = 60;

static SemaphoreHandle_t s_lock;
static bool s_running;
static bool s_decoding;
static uint32_t s_rate;
static uint8_t s_channels;
static uint8_t s_bits;
static uint64_t s_frames;
static uint8_t *s_pcm;
static int s_volume = kDefaultVolume;

static void publish_frames(std::size_t bytes) {
    const uint32_t frame_bytes = (uint32_t)s_channels * (s_bits / 8);
    if (!frame_bytes) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_frames += bytes / frame_bytes;
    xSemaphoreGive(s_lock);
}

static void write_pcm(uint8_t *data, std::size_t len) {
    if (!len) return;
    if (bsp_audio_write(data, len) == ESP_OK) publish_frames(len);
}

#ifdef ESP_PLATFORM

static esp_audio_dec_handle_t s_decoder;

static bool decoder_open(CodecId codec) {
    if (codec != CodecId::Mp3) return false;
    esp_mp3_dec_register();

    esp_audio_dec_cfg_t config = {};
    config.type = ESP_AUDIO_TYPE_MP3;
    if (esp_audio_dec_open(&config, &s_decoder) != ESP_AUDIO_ERR_OK) {
        s_decoder = nullptr;
        return false;
    }
    return true;
}

static void decoder_close() {
    if (!s_decoder) return;
    esp_audio_dec_close(s_decoder);
    s_decoder = nullptr;
}

static void decoder_write(const uint8_t *data, std::size_t len) {
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = const_cast<uint8_t *>(data);
    raw.len = (uint32_t)len;
    raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_PLC;

    while (raw.len > 0) {
        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = s_pcm;
        frame.len = kPcmBytes;
        const esp_audio_err_t err = esp_audio_dec_process(s_decoder, &raw, &frame);
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "mp3 decode: %d", (int)err);
            return;
        }

        esp_audio_dec_info_t info = {};
        if (esp_audio_dec_get_info(s_decoder, &info) == ESP_AUDIO_ERR_OK &&
            info.sample_rate && info.channel &&
            (info.sample_rate != s_rate || info.channel != s_channels)) {
            s_rate = info.sample_rate;
            s_channels = (uint8_t)info.channel;
            s_bits = 16;
            bsp_audio_open(s_rate, s_bits, s_channels);
        }
        write_pcm(s_pcm, frame.decoded_size);
        if (raw.consumed == 0 || raw.consumed > raw.len) return;
        raw.buffer += raw.consumed;
        raw.len -= raw.consumed;
    }
}

#else

static AVCodecContext *s_context;
static AVCodecParserContext *s_parser;
static AVPacket *s_packet;
static AVFrame *s_frame;

static bool decoder_open(CodecId codec) {
    if (codec != CodecId::Mp3) return false;

    const AVCodec *mp3 = avcodec_find_decoder(AV_CODEC_ID_MP3);
    if (!mp3) return false;
    s_context = avcodec_alloc_context3(mp3);
    if (!s_context) return false;
    if (avcodec_open2(s_context, mp3, nullptr) < 0) {
        avcodec_free_context(&s_context);
        return false;
    }
    s_parser = av_parser_init(AV_CODEC_ID_MP3);
    s_packet = av_packet_alloc();
    s_frame = av_frame_alloc();
    return s_parser && s_packet && s_frame;
}

static void decoder_close() {
    if (s_parser) {
        av_parser_close(s_parser);
        s_parser = nullptr;
    }
    if (s_packet) av_packet_free(&s_packet);
    if (s_frame) av_frame_free(&s_frame);
    if (s_context) avcodec_free_context(&s_context);
}

static void emit_frame() {
    const int channels = s_frame->ch_layout.nb_channels;
    if (channels <= 0 || s_frame->nb_samples <= 0) return;

    if ((uint32_t)s_frame->sample_rate != s_rate || (uint8_t)channels != s_channels) {
        s_rate = (uint32_t)s_frame->sample_rate;
        s_channels = (uint8_t)channels;
        s_bits = 16;
        bsp_audio_open(s_rate, s_bits, s_channels);
    }

    const std::size_t bytes = (std::size_t)s_frame->nb_samples * channels * sizeof(int16_t);
    if (bytes > kPcmBytes) return;
    int16_t *out = reinterpret_cast<int16_t *>(s_pcm);

    for (int i = 0; i < s_frame->nb_samples; i++) {
        for (int c = 0; c < channels; c++) {
            float sample = 0.0f;
            switch (s_context->sample_fmt) {
            case AV_SAMPLE_FMT_FLTP:
                sample = reinterpret_cast<const float *>(s_frame->data[c])[i];
                break;
            case AV_SAMPLE_FMT_FLT:
                sample = reinterpret_cast<const float *>(s_frame->data[0])[i * channels + c];
                break;
            case AV_SAMPLE_FMT_S16P:
                sample = reinterpret_cast<const int16_t *>(s_frame->data[c])[i] / 32768.0f;
                break;
            case AV_SAMPLE_FMT_S16:
                sample = reinterpret_cast<const int16_t *>(s_frame->data[0])[i * channels + c] / 32768.0f;
                break;
            default:
                return;
            }
            if (sample > 1.0f) sample = 1.0f;
            if (sample < -1.0f) sample = -1.0f;
            out[i * channels + c] = (int16_t)(sample * 32767.0f);
        }
    }
    write_pcm(s_pcm, bytes);
}

static void decoder_write(const uint8_t *data, std::size_t len) {
    while (len > 0) {
        uint8_t *chunk = nullptr;
        int chunk_len = 0;
        const int used = av_parser_parse2(s_parser, s_context, &chunk, &chunk_len,
                                          data, (int)len, AV_NOPTS_VALUE, AV_NOPTS_VALUE, 0);
        if (used < 0) return;
        data += used;
        len -= (std::size_t)used;
        if (chunk_len <= 0) {
            if (used == 0) return;
            continue;
        }

        s_packet->data = chunk;
        s_packet->size = chunk_len;
        if (avcodec_send_packet(s_context, s_packet) < 0) continue;
        while (avcodec_receive_frame(s_context, s_frame) == 0) emit_frame();
    }
}

#endif

void audio_out_start() {
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    bsp_audio_set_volume(s_volume);
}

bool audio_out_open(CodecId codec, uint32_t rate, uint8_t bits, uint8_t channels,
                    std::string *note) {
    if (!s_lock) return false;
    audio_out_close();

    if (codec == CodecId::None) return false;
    if (codec == CodecId::Unsupported) {
        if (note) *note = "unsupported audio track";
        return false;
    }
    if (!(bsp_audio_get_caps() & BSP_AUDIO_CAP_PCM)) {
        if (note) *note = "no audio output on this board";
        return false;
    }
    if (!rate || !channels) {
        if (note) *note = "audio track has no format";
        return false;
    }
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) bits = 16;

    if (!s_pcm) {
        s_pcm = (uint8_t *)heap_caps_aligned_alloc(64, kPcmBytes, MALLOC_CAP_SPIRAM);
        if (!s_pcm) s_pcm = (uint8_t *)heap_caps_aligned_alloc(64, kPcmBytes, MALLOC_CAP_DEFAULT);
        if (!s_pcm) {
            if (note) *note = "out of memory for the audio buffer";
            return false;
        }
    }

    s_rate = rate;
    s_channels = channels;
    s_bits = codec == CodecId::Pcm ? bits : 16;

    if (codec != CodecId::Pcm && !decoder_open(codec)) {
        if (note) *note = std::string(codec_name(codec)) + " decoder unavailable";
        return false;
    }
    s_decoding = codec != CodecId::Pcm;

    const esp_err_t err = bsp_audio_open(s_rate, s_bits, s_channels);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_open: %s", esp_err_to_name(err));
        decoder_close();
        s_decoding = false;
        if (note) *note = std::string("audio unavailable: ") + esp_err_to_name(err);
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_frames = 0;
    s_running = true;
    xSemaphoreGive(s_lock);

    bsp_audio_set_volume(s_volume);
    ESP_LOGI(TAG, "%s %u Hz %u bit x%u", codec_name(codec), (unsigned)s_rate,
             (unsigned)s_bits, (unsigned)s_channels);
    return true;
}

void audio_out_close() {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool was_running = s_running;
    s_running = false;
    s_frames = 0;
    xSemaphoreGive(s_lock);

    if (s_decoding) {
        decoder_close();
        s_decoding = false;
    }
    if (was_running) bsp_audio_close();
}

void audio_out_write(const uint8_t *data, std::size_t len) {
    if (!s_running || !data || !len) return;
    if (s_decoding) {
        decoder_write(data, len);
        return;
    }
    while (len > 0) {
        const std::size_t take = len < kPcmBytes ? len : kPcmBytes;
        memcpy(s_pcm, data, take);
        write_pcm(s_pcm, take);
        data += take;
        len -= take;
    }
}

void audio_out_flush() {
    if (!s_lock) return;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_frames = 0;
    xSemaphoreGive(s_lock);
}

uint64_t audio_out_position_us() {
    if (!s_lock || !s_rate) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint64_t frames = s_frames;
    const uint32_t rate = s_rate;
    xSemaphoreGive(s_lock);
    return frames * 1000000ull / rate;
}

bool audio_out_running() { return s_running; }

void audio_out_set_volume(int volume) {
    if (volume < 0) volume = 0;
    if (volume > 100) volume = 100;
    s_volume = volume;
    bsp_audio_set_volume(volume);
}

int audio_out_get_volume() { return s_volume; }
