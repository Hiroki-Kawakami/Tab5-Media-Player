/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "decoder.hpp"

#include <cstring>

#ifdef ESP_PLATFORM
extern "C" {
#include "esp_alac_dec.h"
}
#else
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/mem.h>
}
#endif

namespace airplay {

static uint32_t decode_pcm(const uint8_t *data, std::size_t len, uint8_t channels, int16_t *out,
                           uint32_t max_frames) {
    uint32_t frames = (uint32_t)(len / (2u * channels));
    if (frames > max_frames) frames = max_frames;
    for (uint32_t i = 0; i < frames * channels; i++) {
        out[i] = (int16_t)((data[2 * i] << 8) | data[2 * i + 1]);
    }
    return frames;
}

#ifdef ESP_PLATFORM

Decoder::~Decoder() {
    if (handle_) esp_alac_dec_close(handle_);
}

bool Decoder::open(const Format &format) {
    format_ = format;
    if (format.codec != Codec::Alac) return true;
    esp_alac_dec_cfg_t config = ESP_ALAC_DEC_CONFIG_DEFAULT();
    config.codec_spec_info = format_.alac_cookie;
    config.spec_info_len = sizeof(format_.alac_cookie);
    return esp_alac_dec_open(&config, sizeof(config), &handle_) == ESP_AUDIO_ERR_OK;
}

uint32_t Decoder::decode(const uint8_t *data, std::size_t len, int16_t *out,
                         uint32_t max_frames) {
    if (format_.codec == Codec::Pcm) {
        return decode_pcm(data, len, format_.channels, out, max_frames);
    }
    if (!handle_) return 0;
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = const_cast<uint8_t *>(data);
    raw.len = (uint32_t)len;
    esp_audio_dec_out_frame_t frame = {};
    frame.buffer = reinterpret_cast<uint8_t *>(out);
    frame.len = max_frames * format_.channels * sizeof(int16_t);
    esp_audio_dec_info_t info = {};
    if (esp_alac_dec_decode(handle_, &raw, &frame, &info) != ESP_AUDIO_ERR_OK) return 0;
    return frame.decoded_size / (format_.channels * sizeof(int16_t));
}

#else

struct AvDecoder {
    AVCodecContext *context = nullptr;
    AVPacket *packet = nullptr;
    AVFrame *frame = nullptr;

    ~AvDecoder() {
        av_packet_free(&packet);
        av_frame_free(&frame);
        avcodec_free_context(&context);
    }
};

Decoder::~Decoder() { delete static_cast<AvDecoder *>(handle_); }

bool Decoder::open(const Format &format) {
    format_ = format;
    if (format.codec != Codec::Alac) return true;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_ALAC);
    if (!codec) return false;
    auto *decoder = new AvDecoder();
    handle_ = decoder;
    decoder->context = avcodec_alloc_context3(codec);
    decoder->packet = av_packet_alloc();
    decoder->frame = av_frame_alloc();
    if (!decoder->context || !decoder->packet || !decoder->frame) return false;

    static constexpr uint8_t kAtom[12] = { 0, 0, 0, 36, 'a', 'l', 'a', 'c', 0, 0, 0, 0 };
    const int size = sizeof(kAtom) + sizeof(format_.alac_cookie);
    decoder->context->extradata =
        static_cast<uint8_t *>(av_mallocz(size + AV_INPUT_BUFFER_PADDING_SIZE));
    if (!decoder->context->extradata) return false;
    memcpy(decoder->context->extradata, kAtom, sizeof(kAtom));
    memcpy(decoder->context->extradata + sizeof(kAtom), format_.alac_cookie,
           sizeof(format_.alac_cookie));
    decoder->context->extradata_size = size;
    decoder->context->sample_rate = (int)format.rate;
    av_channel_layout_default(&decoder->context->ch_layout, format.channels);
    return avcodec_open2(decoder->context, codec, nullptr) == 0;
}

uint32_t Decoder::decode(const uint8_t *data, std::size_t len, int16_t *out,
                         uint32_t max_frames) {
    if (format_.codec == Codec::Pcm) {
        return decode_pcm(data, len, format_.channels, out, max_frames);
    }
    auto *decoder = static_cast<AvDecoder *>(handle_);
    if (!decoder || av_new_packet(decoder->packet, (int)len) < 0) return 0;
    memcpy(decoder->packet->data, data, len);
    const int sent = avcodec_send_packet(decoder->context, decoder->packet);
    av_packet_unref(decoder->packet);
    if (sent < 0 || avcodec_receive_frame(decoder->context, decoder->frame) < 0) return 0;

    const AVFrame *frame = decoder->frame;
    const int channels = frame->ch_layout.nb_channels;
    if (channels != format_.channels) return 0;
    uint32_t frames = (uint32_t)frame->nb_samples;
    if (frames > max_frames) frames = max_frames;
    for (uint32_t i = 0; i < frames; i++) {
        for (int c = 0; c < channels; c++) {
            int16_t sample = 0;
            if (frame->format == AV_SAMPLE_FMT_S16P) {
                sample = reinterpret_cast<const int16_t *>(frame->data[c])[i];
            } else if (frame->format == AV_SAMPLE_FMT_S16) {
                sample = reinterpret_cast<const int16_t *>(frame->data[0])[i * channels + c];
            }
            out[i * channels + c] = sample;
        }
    }
    return frames;
}

#endif

}  // namespace airplay
