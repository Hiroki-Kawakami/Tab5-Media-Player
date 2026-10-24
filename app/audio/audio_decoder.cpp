/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "audio_decoder.hpp"
#include "audio_output.hpp"
#include "audf_aac.h"
#include "audf_adpcm.h"
#include "audf_alac.h"
#include "audf_convert.h"
#include "audf_flac.h"
#include "audf_mp3.h"
#include "audf_opus.h"
#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <algorithm>
#include <cstring>

static const char *TAG = "audio_decoder";

static constexpr std::size_t kPcmBytes = 64 * 1024;
static constexpr int64_t kOpusPrerollUs = 80000;
static constexpr std::size_t kMpegCarryBytes = 4 * 1024;
static constexpr uint32_t kOutputStackBytes = 4 * 1024;
static constexpr UBaseType_t kOutputPriority = 7;

enum class Mode {
    Pcm,
    Adpcm,
    Aac,
    PendingAac,
    Flac,
    Alac,
    Opus,
    Mpeg,
    Failed,
};

struct DecoderSetup {
    CodecId codec = CodecId::None;
    uint32_t rate = 0;
    uint8_t channels = 0;
    bool adts = false;
    audf_aac_he_t he = AUDF_AAC_HE_OFF;
    std::vector<uint8_t> extradata;
};

static SemaphoreHandle_t s_lock;
static bool s_running;
static Mode s_mode = Mode::Pcm;
static DecoderSetup s_setup;
static uint16_t s_block_align;
static audf_decoder_t *s_adpcm;
static audf_decoder_t *s_aac;
static audf_decoder_t *s_flac;
static audf_decoder_t *s_alac;
static audf_decoder_t *s_opus;
static audf_decoder_t *s_mpeg;
static uint8_t *s_mpeg_carry;
static std::size_t s_mpeg_carry_len;
static uint32_t s_discard;
static uint32_t s_rate;
static uint8_t s_channels;
static uint8_t s_bits;
static AudioContent s_content = AudioContent::Music;
static uint64_t s_frames;

struct OutputBuffer {
    uint8_t *data;
    std::size_t len;
    uint32_t frames;
};

/* Two PCM buffers: the decoder fills s_pcm while the output task writes the
 * other one, so decoding a long frame never stalls the output. */
static uint8_t *s_pcm;
static uint8_t *s_pcm_buffers[2];
static QueueHandle_t s_output_ready;
static QueueHandle_t s_output_free;
static TaskHandle_t s_output_task;
static volatile bool s_output_discard;

static void output_task(void *) {
    for (;;) {
        OutputBuffer buffer;
        xQueueReceive(s_output_ready, &buffer, portMAX_DELAY);
        if (!s_output_discard && audio_output_write(buffer.data, buffer.len) == ESP_OK) {
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_frames += buffer.frames;
            xSemaphoreGive(s_lock);
        }
        xQueueSend(s_output_free, &buffer.data, portMAX_DELAY);
    }
}

static bool output_start() {
    if (s_output_task) return true;
#ifdef ESP_PLATFORM
    BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        output_task, "audio_output", kOutputStackBytes, nullptr, kOutputPriority, &s_output_task,
        tskNO_AFFINITY, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (created != pdPASS) {
        ESP_LOGW(TAG, "audio output task stack falls back to PSRAM");
        created = xTaskCreatePinnedToCoreWithCaps(
            output_task, "audio_output", kOutputStackBytes, nullptr, kOutputPriority,
            &s_output_task, tskNO_AFFINITY, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
#else
    const BaseType_t created = xTaskCreate(output_task, "audio_output", kOutputStackBytes, nullptr,
                                           kOutputPriority, &s_output_task);
#endif
    if (created != pdPASS) {
        s_output_task = nullptr;
        ESP_LOGE(TAG, "cannot create the audio output task");
        return false;
    }
    return true;
}

static bool pcm_alloc() {
    for (uint8_t *&buffer : s_pcm_buffers) {
        if (buffer) continue;
        buffer = (uint8_t *)heap_caps_aligned_alloc(64, kPcmBytes, MALLOC_CAP_SPIRAM);
        if (!buffer) buffer = (uint8_t *)heap_caps_aligned_alloc(64, kPcmBytes, MALLOC_CAP_DEFAULT);
        if (!buffer) return false;
    }
    if (!s_pcm) {
        s_pcm = s_pcm_buffers[0];
        xQueueSend(s_output_free, &s_pcm_buffers[1], 0);
    }
    return true;
}

/* Waits until the output task has handed the other buffer back, playing it
 * or, with discard, dropping it if it has not started. The decoding side must
 * not be inside write_pcm(). */
static void output_drain(bool discard) {
    if (!s_pcm) return;
    s_output_discard = discard;
    uint8_t *other = nullptr;
    xQueueReceive(s_output_free, &other, portMAX_DELAY);
    xQueueSend(s_output_free, &other, 0);
    s_output_discard = false;
}

static void write_pcm(uint8_t *data, std::size_t len) {
    if (!len) return;
    const uint32_t frame_bytes = (uint32_t)s_channels * (s_bits / 8);
    const OutputBuffer buffer = { data, len, frame_bytes ? (uint32_t)(len / frame_bytes) : 0 };
    xQueueSend(s_output_ready, &buffer, portMAX_DELAY);
    xQueueReceive(s_output_free, &s_pcm, portMAX_DELAY);
}

static void follow_format(uint32_t rate, uint8_t channels) {
    if (!rate || !channels || (rate == s_rate && channels == s_channels)) return;
    output_drain(false);
    s_rate = rate;
    s_channels = channels;
    s_bits = 16;
    audio_output_open(s_rate, s_bits, s_channels, s_content);
}

static constexpr uint32_t kAacRates[] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050, 16000, 12000, 11025, 8000, 7350,
};

class BitReader {
public:
    BitReader(const std::vector<uint8_t> &bytes) : bytes_(bytes) {}

    bool read(int count, uint32_t *out) {
        uint32_t value = 0;
        for (int i = 0; i < count; i++) {
            if (position_ >= bytes_.size() * 8) return false;
            const uint8_t bit = (bytes_[position_ / 8] >> (7 - position_ % 8)) & 1;
            value = (value << 1) | bit;
            position_++;
        }
        *out = value;
        return true;
    }

private:
    const std::vector<uint8_t> &bytes_;
    std::size_t position_ = 0;
};

static bool parse_audio_specific_config(const std::vector<uint8_t> &config, uint32_t *rate,
                                        uint8_t *channels) {
    BitReader bits(config);
    uint32_t object = 0;
    uint32_t index = 0;
    uint32_t layout = 0;
    if (!bits.read(5, &object)) return false;
    if (object == 31 && !bits.read(6, &object)) return false;
    if (!bits.read(4, &index)) return false;
    if (index == 15) {
        if (!bits.read(24, rate)) return false;
    } else if (index < sizeof(kAacRates) / sizeof(kAacRates[0])) {
        *rate = kAacRates[index];
    } else {
        return false;
    }
    if (!bits.read(4, &layout)) return false;
    if (layout > 0 && layout <= 2) *channels = (uint8_t)layout;
    return *rate != 0;
}

static std::vector<uint8_t> make_audio_specific_config(uint32_t rate, uint8_t channels) {
    uint32_t index = 15;
    for (uint32_t i = 0; i < sizeof(kAacRates) / sizeof(kAacRates[0]); i++) {
        if (kAacRates[i] == rate) index = i;
    }
    uint64_t bits = 2;
    int count = 5;
    bits = (bits << 4) | index;
    count += 4;
    if (index == 15) {
        bits = (bits << 24) | rate;
        count += 24;
    }
    bits = (bits << 4) | channels;
    count += 4;

    const int padded = (count + 7) / 8 * 8;
    bits <<= padded - count;
    std::vector<uint8_t> config(padded / 8);
    for (std::size_t i = 0; i < config.size(); i++) {
        config[i] = (uint8_t)(bits >> (padded - 8 * (i + 1)));
    }
    return config;
}

static bool is_adts(const uint8_t *data, std::size_t len) {
    return len >= 2 && data[0] == 0xFF && (data[1] & 0xF6) == 0xF0;
}

static void aac_close() {
    audf_decoder_destroy(s_aac);
    s_aac = nullptr;
}

static void open_pending_aac(const uint8_t *data, std::size_t len) {
    s_setup.adts = is_adts(data, len);
    if (!s_setup.adts && s_setup.extradata.empty()) {
        s_setup.extradata = make_audio_specific_config(s_setup.rate, s_setup.channels);
    }
    audf_aac_config_t config = {};
    config.asc = s_setup.adts ? data : s_setup.extradata.data();
    config.asc_len = s_setup.adts ? len : s_setup.extradata.size();
    config.adts = s_setup.adts;
    config.he = s_setup.he;
#ifdef ESP_PLATFORM
    config.alloc_caps = MALLOC_CAP_SPIRAM;
    config.scratch_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
#endif
    const esp_err_t err = audf_aac_decoder_create(&config, &s_aac);
    if (err != ESP_OK ||
        audf_decoder_max_frames(s_aac) * audf_decoder_channels(s_aac) * sizeof(int16_t) > kPcmBytes) {
        ESP_LOGE(TAG, "AAC decoder unavailable: %s", esp_err_to_name(err));
        aac_close();
        s_mode = Mode::Failed;
        return;
    }
    follow_format(audf_aac_decoder_rate(s_aac), audf_decoder_channels(s_aac));
    s_mode = Mode::Aac;
}

static void write_aac(const uint8_t *data, std::size_t len) {
    const std::size_t frame_bytes = audf_decoder_channels(s_aac) * sizeof(int16_t);
    while (len > 0) {
        std::size_t frame_len = len;
        if (s_setup.adts) {
            frame_len = audf_aac_adts_frame_len(data, len);
            if (!frame_len || frame_len > len) return;
        }
        std::size_t frames = 0;
        const esp_err_t err = audf_decoder_decode(s_aac, data, frame_len, s_pcm, &frames);
        if (err == ESP_OK) {
            write_pcm(s_pcm, frames * frame_bytes);
        } else {
            ESP_LOGW(TAG, "AAC decode: %s", esp_err_to_name(err));
        }
        data += frame_len;
        len -= frame_len;
    }
}

static bool adpcm_open(const TrackInfo &track) {
    audf_adpcm_config_t config = {};
    config.channels = track.channels;
    config.block_align = track.block_align;
    if (audf_adpcm_decoder_create(&config, &s_adpcm) != ESP_OK) return false;
    return audf_decoder_max_frames(s_adpcm) * sizeof(int16_t) * track.channels <= kPcmBytes;
}

static void adpcm_close() {
    audf_decoder_destroy(s_adpcm);
    s_adpcm = nullptr;
}

static void write_adpcm(const uint8_t *data, std::size_t len) {
    while (len >= s_block_align) {
        std::size_t frames = 0;
        if (audf_decoder_decode(s_adpcm, data, s_block_align, s_pcm, &frames) == ESP_OK) {
            write_pcm(s_pcm, frames * sizeof(int16_t) * s_channels);
        }
        data += s_block_align;
        len -= s_block_align;
    }
}

static bool lossless_fits(audf_decoder_t *decoder, CodecId codec, std::string *note) {
    const uint8_t channels = audf_decoder_channels(decoder);
    if (channels > 2) {
        if (note) *note = std::string("unsupported ") + codec_name(codec) + " channel count";
        return false;
    }
    if (audf_decoder_max_frames(decoder) * audf_frame_bytes(audf_decoder_fmt(decoder), channels) >
        kPcmBytes) {
        if (note) *note = std::string(codec_name(codec)) + " block size too large";
        return false;
    }
    s_setup.channels = channels;
    return true;
}

static bool flac_open(const TrackInfo &track, std::string *note) {
    audf_flac_config_t config = {};
    config.streaminfo = track.codec_private.data();
    config.streaminfo_len = track.codec_private.size();
#ifdef ESP_PLATFORM
    config.alloc_caps = MALLOC_CAP_SPIRAM;
#endif
    const esp_err_t err = audf_flac_decoder_create(&config, &s_flac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FLAC decoder: %s", esp_err_to_name(err));
        if (note) *note = "unsupported FLAC format";
        return false;
    }
    if (!lossless_fits(s_flac, CodecId::Flac, note)) return false;
    s_setup.rate = audf_flac_decoder_rate(s_flac);
    return true;
}

static void flac_close() {
    audf_decoder_destroy(s_flac);
    s_flac = nullptr;
}

static bool alac_open(const TrackInfo &track, std::string *note) {
    audf_alac_config_t config = {};
    config.cookie = track.codec_private.data();
    config.cookie_len = track.codec_private.size();
#ifdef ESP_PLATFORM
    config.alloc_caps = MALLOC_CAP_SPIRAM;
#endif
    const esp_err_t err = audf_alac_decoder_create(&config, &s_alac);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ALAC decoder: %s", esp_err_to_name(err));
        if (note) *note = "unsupported ALAC format";
        return false;
    }
    if (!lossless_fits(s_alac, CodecId::Alac, note)) return false;
    s_setup.rate = audf_alac_decoder_rate(s_alac);
    return true;
}

static void alac_close() {
    audf_decoder_destroy(s_alac);
    s_alac = nullptr;
}

static bool opus_open(const TrackInfo &track, std::string *note) {
    std::vector<uint8_t> head = track.codec_private;
    if (head.size() < AUDF_OPUS_HEAD_BYTES || memcmp(head.data(), "OpusHead", 8) != 0) {
        head.assign(AUDF_OPUS_HEAD_BYTES, 0);
        memcpy(head.data(), "OpusHead", 8);
        head[8] = 1;
        head[9] = track.channels;
    }
    audf_opus_config_t config = {};
    config.head = head.data();
    config.head_len = head.size();
#ifdef ESP_PLATFORM
    config.alloc_caps = MALLOC_CAP_SPIRAM;
#endif
    const esp_err_t err = audf_opus_decoder_create(&config, &s_opus);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Opus decoder: %s", esp_err_to_name(err));
        if (note) {
            *note = err == ESP_ERR_NOT_SUPPORTED ? "unsupported Opus channel mapping"
                                                 : "Opus decoder unavailable";
        }
        return false;
    }
    s_setup.rate = AUDF_OPUS_RATE;
    s_setup.channels = audf_decoder_channels(s_opus);
    s_discard = audf_opus_decoder_pre_skip(s_opus);
    return true;
}

static void opus_close() {
    audf_decoder_destroy(s_opus);
    s_opus = nullptr;
}

static void write_opus(const uint8_t *data, std::size_t len) {
    std::size_t frames = 0;
    const esp_err_t err = audf_decoder_decode(s_opus, data, len, s_pcm, &frames);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Opus decode: %s", esp_err_to_name(err));
        return;
    }
    const std::size_t frame_bytes = (std::size_t)s_channels * sizeof(int16_t);
    if (s_discard >= frames) {
        s_discard -= frames;
        return;
    }
    if (s_discard) {
        memmove(s_pcm, s_pcm + s_discard * frame_bytes, (frames - s_discard) * frame_bytes);
        frames -= s_discard;
        s_discard = 0;
    }
    write_pcm(s_pcm, frames * frame_bytes);
}

static bool mpeg_open(const TrackInfo &track, std::string *note) {
    if (track.channels > 2) {
        if (note) *note = std::string("unsupported ") + codec_name(track.codec) + " channel count";
        return false;
    }
    if (!s_mpeg_carry) {
        s_mpeg_carry = static_cast<uint8_t *>(heap_caps_malloc(kMpegCarryBytes, MALLOC_CAP_SPIRAM));
        if (!s_mpeg_carry) {
            s_mpeg_carry = static_cast<uint8_t *>(heap_caps_malloc(kMpegCarryBytes, MALLOC_CAP_DEFAULT));
        }
        if (!s_mpeg_carry) {
            if (note) *note = "out of memory for the audio buffer";
            return false;
        }
    }
    s_mpeg_carry_len = 0;
    audf_mp3_config_t config = {};
    config.channels = track.channels;
#ifdef ESP_PLATFORM
    config.alloc_caps = MALLOC_CAP_SPIRAM;
#endif
    const esp_err_t err = audf_mp3_decoder_create(&config, &s_mpeg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s decoder: %s", codec_name(track.codec), esp_err_to_name(err));
        if (note) *note = std::string(codec_name(track.codec)) + " decoder unavailable";
        return false;
    }
    return true;
}

static void mpeg_close() {
    audf_decoder_destroy(s_mpeg);
    s_mpeg = nullptr;
    heap_caps_free(s_mpeg_carry);
    s_mpeg_carry = nullptr;
    s_mpeg_carry_len = 0;
}

static void decode_mpeg_frame(const uint8_t *frame, const audf_mp3_header_t &header) {
    const std::size_t frame_bytes = (std::size_t)s_channels * sizeof(int16_t);
    std::size_t frames = 0;
    const esp_err_t err = audf_decoder_decode(s_mpeg, frame, header.bytes, s_pcm, &frames);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s decode: %s", codec_name(s_setup.codec), esp_err_to_name(err));
        frames = header.frames;
        memset(s_pcm, 0, frames * frame_bytes);
    }
    follow_format(header.rate, s_channels);
    write_pcm(s_pcm, frames * frame_bytes);
}

/* AVI packets cut MP3 frames anywhere, so frames are cut out here. */
static void write_mpeg(const uint8_t *data, std::size_t len) {
    while (len > 0) {
        const std::size_t take = std::min(len, kMpegCarryBytes - s_mpeg_carry_len);
        memcpy(s_mpeg_carry + s_mpeg_carry_len, data, take);
        s_mpeg_carry_len += take;
        data += take;
        len -= take;

        std::size_t pos = 0;
        while (pos + AUDF_MP3_HEADER_BYTES <= s_mpeg_carry_len) {
            audf_mp3_header_t header;
            if (!audf_mp3_parse_header(s_mpeg_carry + pos, &header)) {
                pos++;
                continue;
            }
            if (pos + header.bytes > s_mpeg_carry_len) break;
            decode_mpeg_frame(s_mpeg_carry + pos, header);
            pos += header.bytes;
        }
        s_mpeg_carry_len -= pos;
        memmove(s_mpeg_carry, s_mpeg_carry + pos, s_mpeg_carry_len);
    }
}

static void write_lossless(audf_decoder_t *decoder, const uint8_t *data, std::size_t len) {
    std::size_t frames = 0;
    const esp_err_t err = audf_decoder_decode(decoder, data, len, s_pcm, &frames);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s decode: %s", codec_name(s_setup.codec), esp_err_to_name(err));
        return;
    }
    const std::size_t samples = frames * s_channels;
    if (audf_decoder_fmt(decoder) == AUDF_FMT_S32) {
        audf_convert_to_pcm(s_pcm, AUDF_FMT_S32, s_pcm, AUDF_PCM_S24, samples);
        write_pcm(s_pcm, samples * 3);
    } else {
        write_pcm(s_pcm, samples * sizeof(int16_t));
    }
}

static bool prepare(const TrackInfo &track, audf_aac_he_t aac_he, std::string *note) {
    s_setup = {};
    s_setup.codec = track.codec;
    s_setup.he = aac_he;
    s_setup.rate = track.sample_rate;
    s_setup.channels = track.channels;
    s_block_align = track.block_align;

    switch (track.codec) {
    case CodecId::Pcm:
        s_mode = Mode::Pcm;
        return true;
    case CodecId::AdpcmIma:
        if (!adpcm_open(track)) {
            adpcm_close();
            if (note) *note = "unsupported IMA ADPCM format";
            return false;
        }
        s_mode = Mode::Adpcm;
        return true;
    case CodecId::Aac:
        s_setup.extradata = track.codec_private;
        if (!s_setup.extradata.empty() &&
            !parse_audio_specific_config(s_setup.extradata, &s_setup.rate, &s_setup.channels)) {
            if (note) *note = "unsupported AAC configuration";
            return false;
        }
        s_mode = Mode::PendingAac;
        return true;
    case CodecId::Flac:
        if (!flac_open(track, note)) {
            flac_close();
            return false;
        }
        s_mode = Mode::Flac;
        return true;
    case CodecId::Alac:
        if (!alac_open(track, note)) {
            alac_close();
            return false;
        }
        s_mode = Mode::Alac;
        return true;
    case CodecId::Opus:
        if (!opus_open(track, note)) {
            opus_close();
            return false;
        }
        s_mode = Mode::Opus;
        return true;
    case CodecId::Mp3:
    case CodecId::Mp2:
        if (!mpeg_open(track, note)) {
            mpeg_close();
            return false;
        }
        s_mode = Mode::Mpeg;
        return true;
    default:
        if (note) *note = std::string(codec_name(track.codec)) + " decoder unavailable";
        return false;
    }
}

void audio_decoder_start() {
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    s_output_ready = xQueueCreate(1, sizeof(OutputBuffer));
    s_output_free = xQueueCreate(2, sizeof(uint8_t *));
}

bool audio_decoder_open(const TrackInfo &track, audf_aac_he_t aac_he, AudioContent content,
                        std::string *note) {
    if (!s_lock) return false;
    audio_decoder_close();

    if (track.codec == CodecId::None) return false;
    if (track.codec == CodecId::Unsupported) {
        if (note) *note = "unsupported audio track";
        return false;
    }
    if (!(bsp_audio_get_caps() & BSP_AUDIO_CAP_PCM)) {
        if (note) *note = "no audio output on this board";
        return false;
    }
    if (!track.sample_rate || !track.channels) {
        if (note) *note = "audio track has no format";
        return false;
    }

    if (!pcm_alloc()) {
        if (note) *note = "out of memory for the audio buffer";
        return false;
    }
    if (!output_start()) {
        if (note) *note = "audio output task unavailable";
        return false;
    }

    if (!prepare(track, aac_he, note)) {
        s_mode = Mode::Pcm;
        return false;
    }

    uint8_t bits = track.bits;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) bits = 16;
    s_rate = s_setup.rate;
    s_channels = s_setup.channels;
    s_bits = track.codec == CodecId::Pcm ? bits : 16;
    if (s_mode == Mode::Flac && audf_decoder_fmt(s_flac) == AUDF_FMT_S32) s_bits = 24;
    if (s_mode == Mode::Alac && audf_decoder_fmt(s_alac) == AUDF_FMT_S32) s_bits = 24;

    s_content = content;
    const esp_err_t err = audio_output_open(s_rate, s_bits, s_channels, s_content);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audio_output_open: %s", esp_err_to_name(err));
        mpeg_close();
        adpcm_close();
        aac_close();
        flac_close();
        alac_close();
        opus_close();
        s_mode = Mode::Pcm;
        if (note) *note = std::string("audio unavailable: ") + esp_err_to_name(err);
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_frames = 0;
    s_running = true;
    xSemaphoreGive(s_lock);

    audio_output_apply_volume(audio_output_volume());
    ESP_LOGI(TAG, "%s %u Hz %u bit x%u", codec_name(track.codec), (unsigned)s_rate,
             (unsigned)s_bits, (unsigned)s_channels);
    return true;
}

void audio_decoder_close() {
    if (!s_lock) return;
    output_drain(false);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool was_running = s_running;
    s_running = false;
    s_frames = 0;
    xSemaphoreGive(s_lock);

    mpeg_close();
    adpcm_close();
    aac_close();
    flac_close();
    alac_close();
    opus_close();
    s_mode = Mode::Pcm;
    if (was_running) audio_output_close();
}

void audio_decoder_write(const uint8_t *data, std::size_t len) {
    if (!s_running || !data || !len) return;
    if (s_mode == Mode::PendingAac) open_pending_aac(data, len);

    switch (s_mode) {
    case Mode::Mpeg:
        write_mpeg(data, len);
        return;
    case Mode::Aac:
        write_aac(data, len);
        return;
    case Mode::Flac:
        write_lossless(s_flac, data, len);
        return;
    case Mode::Alac:
        write_lossless(s_alac, data, len);
        return;
    case Mode::Opus:
        write_opus(data, len);
        return;
    case Mode::Adpcm:
        write_adpcm(data, len);
        return;
    case Mode::Pcm:
        break;
    default:
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

int64_t audio_decoder_preroll_us() {
    return s_mode == Mode::Opus ? kOpusPrerollUs : 0;
}

void audio_decoder_flush(int64_t discard_us, bool start) {
    if (!s_lock) return;
    output_drain(true);
    if (s_mode == Mode::Mpeg) {
        audf_decoder_reset(s_mpeg);
        s_mpeg_carry_len = 0;
    }
    if (s_mode == Mode::Aac) audf_decoder_reset(s_aac);
    if (s_mode == Mode::Opus) {
        audf_decoder_reset(s_opus);
        s_discard = (uint32_t)((discard_us > 0 ? discard_us : 0) * AUDF_OPUS_RATE / 1000000);
        if (start) s_discard += audf_opus_decoder_pre_skip(s_opus);
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_frames = 0;
    xSemaphoreGive(s_lock);
}

uint64_t audio_decoder_position_us() {
    if (!s_lock || !s_rate) return 0;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const uint64_t frames = s_frames;
    const uint32_t rate = s_rate;
    xSemaphoreGive(s_lock);
    return frames * 1000000ull / rate;
}

bool audio_decoder_running() { return s_running; }
