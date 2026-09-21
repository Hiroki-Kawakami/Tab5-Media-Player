/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "riff.h"

#include <string.h>

#include "esp_heap_caps.h"

bool riff_read(media_buffer_t *reader, void *out, size_t size) {
    return mb_read(reader, out, size) == size;
}

bool riff_next_chunk(media_buffer_t *reader, off_t limit, riff_chunk_t *chunk, off_t *body,
                     off_t *next) {
    if (mb_tell(reader) + (off_t)sizeof(*chunk) > limit) return false;
    if (!riff_read(reader, chunk, sizeof(*chunk))) return false;
    *body = mb_tell(reader);
    *next = *body + chunk->size + (chunk->size & 1);
    if (*next < *body) return false;
    return true;
}

riff_audio_codec_t riff_audio_codec_of_tag(uint16_t format_tag) {
    switch (format_tag) {
    case RIFF_WAVE_FORMAT_PCM:
        return RIFF_AUDIO_CODEC_PCM;
    case RIFF_WAVE_FORMAT_MP3:
        return RIFF_AUDIO_CODEC_MP3;
    case RIFF_WAVE_FORMAT_IMA_ADPCM:
        return RIFF_AUDIO_CODEC_ADPCM_IMA;
    case RIFF_WAVE_FORMAT_AAC:
    case RIFF_WAVE_FORMAT_AAC_ADTS:
    case RIFF_WAVE_FORMAT_AAC_FAAD:
        return RIFF_AUDIO_CODEC_AAC;
    default:
        return RIFF_AUDIO_CODEC_UNSUPPORTED;
    }
}

const char *riff_audio_codec_name(riff_audio_codec_t codec) {
    switch (codec) {
    case RIFF_AUDIO_CODEC_NONE: return "none";
    case RIFF_AUDIO_CODEC_PCM: return "PCM";
    case RIFF_AUDIO_CODEC_MP3: return "MP3";
    case RIFF_AUDIO_CODEC_ADPCM_IMA: return "IMA ADPCM";
    case RIFF_AUDIO_CODEC_AAC: return "AAC";
    default: return "unsupported";
    }
}

static uint8_t *read_extra(media_buffer_t *reader, uint32_t available, uint32_t *size) {
    *size = 0;
    uint16_t extra_size = 0;
    if (available < sizeof(extra_size) || !riff_read(reader, &extra_size, sizeof(extra_size))) {
        return NULL;
    }
    available -= sizeof(extra_size);
    if (extra_size > available) extra_size = (uint16_t)available;
    if (extra_size == 0 || extra_size > RIFF_WAVE_EXTRA_LIMIT) return NULL;

    uint8_t *extra = heap_caps_malloc(extra_size, MALLOC_CAP_DEFAULT);
    if (!extra) return NULL;
    if (!riff_read(reader, extra, extra_size)) {
        heap_caps_free(extra);
        return NULL;
    }
    *size = extra_size;
    return extra;
}

bool riff_read_wave_format(media_buffer_t *reader, uint32_t chunk_size, riff_audio_format_t *out) {
    memset(out, 0, sizeof(*out));
    riff_wave_format_t wave;
    if (chunk_size < sizeof(wave) || !riff_read(reader, &wave, sizeof(wave))) return false;

    out->codec = riff_audio_codec_of_tag(wave.format_tag);
    out->channels = (uint8_t)wave.channels;
    out->sample_rate = wave.samples_per_sec;
    out->bitrate_bps = wave.avg_bytes_per_sec * 8;
    out->bits_per_sample = wave.bits_per_sample ? (uint8_t)wave.bits_per_sample : 16;
    out->block_align = wave.block_align;

    const uint32_t available = chunk_size - (uint32_t)sizeof(wave);
    if (wave.format_tag == RIFF_WAVE_FORMAT_EXTENSIBLE) {
        /* cbSize, valid bits, channel mask, then the subformat GUID. */
        uint8_t tail[24];
        const uint32_t want = sizeof(tail);
        if (available < want || !riff_read(reader, tail, want)) {
            out->codec = RIFF_AUDIO_CODEC_UNSUPPORTED;
            return true;
        }
        out->codec = riff_audio_codec_of_tag((uint16_t)(tail[8] | (tail[9] << 8)));
        return true;
    }
    out->extra = read_extra(reader, available, &out->extra_size);
    return true;
}
