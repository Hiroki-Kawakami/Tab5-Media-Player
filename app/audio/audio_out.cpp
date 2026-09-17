/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "audio_out.hpp"
#include "ima_adpcm.hpp"
#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include <cstring>

#ifdef ESP_PLATFORM
extern "C" {
#include "esp_aac_dec.h"
#include "esp_audio_dec.h"
#include "esp_mp3_dec.h"
#include "esp_opus_dec.h"
}
#else
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mem.h>
#include <libavutil/samplefmt.h>
}
#endif

static const char *TAG = "audio_out";

static constexpr std::size_t kPcmBytes = 64 * 1024;
static constexpr int kDefaultVolume = 60;
static constexpr uint32_t kOpusRate = 48000;

enum class Mode {
    Pcm,
    Adpcm,
    Decoder,
    PendingDecoder,
    Failed,
};

struct DecoderSetup {
    CodecId codec = CodecId::None;
    uint32_t rate = 0;
    uint8_t channels = 0;
    bool adts = false;
    bool sbr = true;
    std::vector<uint8_t> extradata;
};

static SemaphoreHandle_t s_lock;
static bool s_running;
static Mode s_mode = Mode::Pcm;
static DecoderSetup s_setup;
static uint16_t s_block_align;
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

static void follow_format(uint32_t rate, uint8_t channels) {
    if (!rate || !channels || (rate == s_rate && channels == s_channels)) return;
    s_rate = rate;
    s_channels = channels;
    s_bits = 16;
    bsp_audio_open(s_rate, s_bits, s_channels);
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

static bool prepare_opus(const TrackInfo &track, DecoderSetup *setup, std::string *note) {
    const std::vector<uint8_t> &head = track.codec_private;
    setup->rate = kOpusRate;
    setup->channels = track.channels;
    if (head.size() >= 19 && memcmp(head.data(), "OpusHead", 8) == 0) {
        setup->channels = head[9];
        if (head[18] != 0) {
            if (note) *note = "unsupported Opus channel mapping";
            return false;
        }
    }
    if (setup->channels == 0 || setup->channels > 2) {
        if (note) *note = "unsupported Opus channel count";
        return false;
    }
    setup->extradata = head;
    return true;
}

#ifdef ESP_PLATFORM

static esp_audio_dec_handle_t s_decoder;

static bool decoder_open() {
    const DecoderSetup &setup = s_setup;
    esp_audio_dec_cfg_t config = {};
    esp_aac_dec_cfg_t aac = {};
    esp_opus_dec_cfg_t opus = {};
    switch (setup.codec) {
    case CodecId::Mp3:
        esp_mp3_dec_register();
        config.type = ESP_AUDIO_TYPE_MP3;
        break;
    case CodecId::Aac:
        esp_aac_dec_register();
        config.type = ESP_AUDIO_TYPE_AAC;
        aac.sample_rate = (int32_t)setup.rate;
        aac.channel = setup.channels;
        aac.bits_per_sample = 16;
        aac.no_adts_header = !setup.adts;
        aac.aac_plus_enable = setup.sbr;
        config.cfg = &aac;
        config.cfg_sz = sizeof(aac);
        break;
    case CodecId::Opus:
        esp_opus_dec_register();
        config.type = ESP_AUDIO_TYPE_OPUS;
        opus.sample_rate = setup.rate;
        opus.channel = setup.channels;
        opus.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_INVALID;
        opus.self_delimited = false;
        config.cfg = &opus;
        config.cfg_sz = sizeof(opus);
        break;
    default:
        return false;
    }
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

static void decoder_reset() {
    if (s_decoder) esp_audio_dec_reset(s_decoder);
}

static void decoder_write(const uint8_t *data, std::size_t len) {
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = const_cast<uint8_t *>(data);
    raw.len = (uint32_t)len;
    raw.frame_recover = s_setup.codec == CodecId::Mp3 ? ESP_AUDIO_DEC_RECOVERY_PLC
                                                      : ESP_AUDIO_DEC_RECOVERY_NONE;

    while (raw.len > 0) {
        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = s_pcm;
        frame.len = kPcmBytes;
        const esp_audio_err_t err = esp_audio_dec_process(s_decoder, &raw, &frame);
        if (err != ESP_AUDIO_ERR_OK) {
            ESP_LOGW(TAG, "%s decode: %d", codec_name(s_setup.codec), (int)err);
            return;
        }

        esp_audio_dec_info_t info = {};
        if (esp_audio_dec_get_info(s_decoder, &info) == ESP_AUDIO_ERR_OK) {
            follow_format(info.sample_rate, (uint8_t)info.channel);
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

static AVCodecID codec_id_of(CodecId codec) {
    switch (codec) {
    case CodecId::Mp3: return AV_CODEC_ID_MP3;
    case CodecId::Aac: return AV_CODEC_ID_AAC;
    case CodecId::Opus: return AV_CODEC_ID_OPUS;
    default: return AV_CODEC_ID_NONE;
    }
}

static bool uses_parser() {
    return s_setup.codec == CodecId::Mp3 || (s_setup.codec == CodecId::Aac && s_setup.adts);
}

static bool decoder_open() {
    const DecoderSetup &setup = s_setup;
    const AVCodecID id = codec_id_of(setup.codec);
    if (id == AV_CODEC_ID_NONE) return false;
    const AVCodec *codec = avcodec_find_decoder(id);
    if (!codec) return false;
    s_context = avcodec_alloc_context3(codec);
    if (!s_context) return false;

    s_context->sample_rate = (int)setup.rate;
    av_channel_layout_default(&s_context->ch_layout, setup.channels);
    if (!setup.extradata.empty()) {
        s_context->extradata = static_cast<uint8_t *>(
            av_mallocz(setup.extradata.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!s_context->extradata) return false;
        memcpy(s_context->extradata, setup.extradata.data(), setup.extradata.size());
        s_context->extradata_size = (int)setup.extradata.size();
    }
    if (avcodec_open2(s_context, codec, nullptr) < 0) return false;

    if (uses_parser()) {
        s_parser = av_parser_init(id);
        if (!s_parser) return false;
    }
    s_packet = av_packet_alloc();
    s_frame = av_frame_alloc();
    return s_packet && s_frame;
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

static void decoder_reset() {
    if (!s_context) return;
    avcodec_flush_buffers(s_context);
    if (s_parser) {
        av_parser_close(s_parser);
        s_parser = av_parser_init(codec_id_of(s_setup.codec));
    }
}

static void emit_frame() {
    const int channels = s_frame->ch_layout.nb_channels;
    if (channels <= 0 || s_frame->nb_samples <= 0) return;
    follow_format((uint32_t)s_frame->sample_rate, (uint8_t)channels);

    const std::size_t bytes = (std::size_t)s_frame->nb_samples * channels * sizeof(int16_t);
    if (bytes > kPcmBytes) return;
    int16_t *out = reinterpret_cast<int16_t *>(s_pcm);

    for (int i = 0; i < s_frame->nb_samples; i++) {
        for (int c = 0; c < channels; c++) {
            float sample = 0.0f;
            switch (s_frame->format) {
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

static void send_packet(const uint8_t *data, int len) {
    if (av_new_packet(s_packet, len) < 0) return;
    memcpy(s_packet->data, data, (std::size_t)len);
    const int err = avcodec_send_packet(s_context, s_packet);
    av_packet_unref(s_packet);
    if (err < 0) return;
    while (avcodec_receive_frame(s_context, s_frame) == 0) emit_frame();
}

static void decoder_write(const uint8_t *data, std::size_t len) {
    if (!uses_parser()) {
        send_packet(data, (int)len);
        return;
    }
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
        send_packet(chunk, chunk_len);
    }
}

#endif

static void open_pending_decoder(const uint8_t *data, std::size_t len) {
    s_setup.adts = is_adts(data, len);
    if (!s_setup.adts && s_setup.extradata.empty()) {
        s_setup.extradata = make_audio_specific_config(s_setup.rate, s_setup.channels);
    }
    if (decoder_open()) {
        s_mode = Mode::Decoder;
        return;
    }
    ESP_LOGE(TAG, "%s decoder unavailable", codec_name(s_setup.codec));
    decoder_close();
    s_mode = Mode::Failed;
}

static void write_adpcm(const uint8_t *data, std::size_t len) {
    const std::size_t block = s_block_align ? s_block_align : len;
    const std::size_t max_frames = kPcmBytes / (sizeof(int16_t) * s_channels);
    int16_t *out = reinterpret_cast<int16_t *>(s_pcm);
    while (len >= block) {
        const std::size_t frames = ima_adpcm_decode(data, block, s_channels, out, max_frames);
        write_pcm(s_pcm, frames * sizeof(int16_t) * s_channels);
        data += block;
        len -= block;
    }
}

static bool prepare(const TrackInfo &track, bool aac_sbr, std::string *note) {
    s_setup = {};
    s_setup.codec = track.codec;
    s_setup.sbr = aac_sbr;
    s_setup.rate = track.sample_rate;
    s_setup.channels = track.channels;
    s_block_align = track.block_align;

    switch (track.codec) {
    case CodecId::Pcm:
        s_mode = Mode::Pcm;
        return true;
    case CodecId::AdpcmIma:
        if (track.channels > kImaAdpcmMaxChannels) {
            if (note) *note = "unsupported IMA ADPCM channel count";
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
        s_mode = Mode::PendingDecoder;
        return true;
    case CodecId::Opus:
        if (!prepare_opus(track, &s_setup, note)) return false;
        break;
    case CodecId::Mp3:
        break;
    default:
        if (note) *note = std::string(codec_name(track.codec)) + " decoder unavailable";
        return false;
    }

    if (!decoder_open()) {
        decoder_close();
        if (note) *note = std::string(codec_name(track.codec)) + " decoder unavailable";
        return false;
    }
    s_mode = Mode::Decoder;
    return true;
}

void audio_out_start() {
    if (s_lock) return;
    s_lock = xSemaphoreCreateMutex();
    bsp_audio_set_volume(s_volume);
}

bool audio_out_open(const TrackInfo &track, bool aac_sbr, std::string *note) {
    if (!s_lock) return false;
    audio_out_close();

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

    if (!s_pcm) {
        s_pcm = (uint8_t *)heap_caps_aligned_alloc(64, kPcmBytes, MALLOC_CAP_SPIRAM);
        if (!s_pcm) s_pcm = (uint8_t *)heap_caps_aligned_alloc(64, kPcmBytes, MALLOC_CAP_DEFAULT);
        if (!s_pcm) {
            if (note) *note = "out of memory for the audio buffer";
            return false;
        }
    }

    if (!prepare(track, aac_sbr, note)) {
        s_mode = Mode::Pcm;
        return false;
    }

    uint8_t bits = track.bits;
    if (bits != 8 && bits != 16 && bits != 24 && bits != 32) bits = 16;
    s_rate = s_setup.rate;
    s_channels = s_setup.channels;
    s_bits = track.codec == CodecId::Pcm ? bits : 16;

    const esp_err_t err = bsp_audio_open(s_rate, s_bits, s_channels);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "bsp_audio_open: %s", esp_err_to_name(err));
        decoder_close();
        s_mode = Mode::Pcm;
        if (note) *note = std::string("audio unavailable: ") + esp_err_to_name(err);
        return false;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_frames = 0;
    s_running = true;
    xSemaphoreGive(s_lock);

    bsp_audio_set_volume(s_volume);
    ESP_LOGI(TAG, "%s %u Hz %u bit x%u", codec_name(track.codec), (unsigned)s_rate,
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

    decoder_close();
    s_mode = Mode::Pcm;
    if (was_running) bsp_audio_close();
}

void audio_out_write(const uint8_t *data, std::size_t len) {
    if (!s_running || !data || !len) return;
    if (s_mode == Mode::PendingDecoder) open_pending_decoder(data, len);

    switch (s_mode) {
    case Mode::Decoder:
        decoder_write(data, len);
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

void audio_out_flush() {
    if (!s_lock) return;
    if (s_mode == Mode::Decoder) decoder_reset();
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
