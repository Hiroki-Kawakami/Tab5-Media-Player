/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "wav_demux.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "riff.h"

static const char *TAG = "wav_demux";

#define WAV_PACKET_BYTES (32 * 1024)

struct wav_demux {
    media_buffer_t *reader;
    wav_info_t info;
    uint8_t *extra;
    off_t data_start;
    off_t data_end;
    off_t cursor;
    uint32_t unit_bytes;
    uint32_t unit_samples;
};

static int64_t units_to_us(const wav_demux_t *demux, uint64_t units) {
    return (int64_t)((units * demux->unit_samples * 1000000ull) / demux->info.sample_rate);
}

static uint64_t us_to_units(const wav_demux_t *demux, int64_t us) {
    if (us <= 0) return 0;
    const uint64_t samples = (uint64_t)us * demux->info.sample_rate / 1000000ull;
    return samples / demux->unit_samples;
}

static const char *describe_format(wav_demux_t *demux, const riff_audio_format_t *format) {
    if (!format->sample_rate || !format->channels) return "this WAV has no audio format";

    switch (format->codec) {
    case RIFF_AUDIO_CODEC_PCM:
        if (format->bits_per_sample != 16 && format->bits_per_sample != 24 &&
            format->bits_per_sample != 32) {
            return "only 16, 24 and 32-bit PCM WAV files are supported";
        }
        demux->unit_bytes = (uint32_t)format->channels * (format->bits_per_sample / 8);
        demux->unit_samples = 1;
        break;
    case RIFF_AUDIO_CODEC_ADPCM_IMA: {
        const uint16_t block = format->block_align;
        if (!block || block % (4 * format->channels) != 0) return "unusable IMA ADPCM block size";
        demux->unit_bytes = block;
        demux->unit_samples = (uint32_t)(block / format->channels - 4) * 2 + 1;
        break;
    }
    default:
        return "unsupported audio codec in this WAV";
    }
    return NULL;
}

wav_demux_t *wav_demux_open(const char *path, const media_arena_t *arena, const char **error) {
    const char *ignored = NULL;
    if (!error) error = &ignored;
    *error = NULL;

    wav_demux_t *demux = heap_caps_calloc(1, sizeof(*demux), MALLOC_CAP_DEFAULT);
    if (!demux) {
        *error = "out of memory";
        return NULL;
    }
    demux->reader = mb_open(path, arena);
    if (!demux->reader) {
        *error = "cannot open the file";
        heap_caps_free(demux);
        return NULL;
    }

    riff_chunk_t riff;
    uint32_t type = 0;
    if (!riff_read(demux->reader, &riff, sizeof(riff)) ||
        !riff_read(demux->reader, &type, sizeof(type)) || riff.fourcc != RIFF_ID_RIFF ||
        type != RIFF_TYPE_WAVE) {
        *error = "not a WAV file";
        wav_demux_close(demux);
        return NULL;
    }

    const off_t file_end = mb_size(demux->reader);
    riff_audio_format_t format;
    memset(&format, 0, sizeof(format));
    bool have_format = false;

    riff_chunk_t chunk;
    off_t body = 0;
    off_t next = 0;
    while (riff_next_chunk(demux->reader, file_end, &chunk, &body, &next)) {
        if (chunk.fourcc == RIFF_ID_fmt && !have_format) {
            have_format = riff_read_wave_format(demux->reader, chunk.size, &format);
        } else if (chunk.fourcc == RIFF_ID_data && !demux->data_start) {
            demux->data_start = body;
            demux->data_end = body + chunk.size;
            if (chunk.size == 0 || demux->data_end > file_end) demux->data_end = file_end;
            next = demux->data_end + (demux->data_end & 1);
        } else if (chunk.fourcc == RIFF_ID_LIST) {
            uint32_t list_type = 0;
            if (riff_read(demux->reader, &list_type, sizeof(list_type)) &&
                list_type == RIFF_ID_INFO) {
                riff_read_info(demux->reader, next, &demux->info.tags);
            }
        } else if (chunk.fourcc == RIFF_ID_id3 || chunk.fourcc == RIFF_ID_ID3) {
            media_tags_read_id3v2(demux->reader, body, NULL, &demux->info.tags);
        }
        if (next > file_end) break;
        mb_seek(demux->reader, next);
    }

    if (!have_format || !demux->data_start || demux->data_end <= demux->data_start) {
        *error = have_format ? "no audio data in this WAV" : "no format in this WAV";
        heap_caps_free(format.extra);
        wav_demux_close(demux);
        return NULL;
    }

    demux->info.codec = format.codec;
    demux->info.sample_rate = format.sample_rate;
    demux->info.bitrate_bps = format.bitrate_bps;
    demux->info.channels = format.channels;
    demux->info.bits_per_sample = format.bits_per_sample;
    demux->info.block_align = format.block_align;

    const char *failure = describe_format(demux, &format);
    if (failure) {
        *error = failure;
        heap_caps_free(format.extra);
        wav_demux_close(demux);
        return NULL;
    }
    demux->extra = format.extra;
    demux->info.codec_private = format.extra;
    demux->info.codec_private_size = format.extra_size;

    const uint64_t units = (uint64_t)(demux->data_end - demux->data_start) / demux->unit_bytes;
    demux->info.duration_us = units_to_us(demux, units);
    if (!demux->info.bitrate_bps) {
        demux->info.bitrate_bps =
            demux->info.sample_rate / demux->unit_samples * demux->unit_bytes * 8;
    }
    demux->cursor = demux->data_start;

    ESP_LOGI(TAG, "%s: %s %u Hz %u bit x%u, %lld us", path,
             riff_audio_codec_name(demux->info.codec), (unsigned)demux->info.sample_rate,
             (unsigned)demux->info.bits_per_sample, (unsigned)demux->info.channels,
             (long long)demux->info.duration_us);

    mb_seek(demux->reader, demux->cursor);
    mb_set_readahead(demux->reader, true);
    return demux;
}

void wav_demux_close(wav_demux_t *demux) {
    if (!demux) return;
    media_tags_free(&demux->info.tags);
    if (demux->reader) mb_close(demux->reader);
    heap_caps_free(demux->extra);
    heap_caps_free(demux);
}

const wav_info_t *wav_demux_info(const wav_demux_t *demux) {
    return demux ? &demux->info : NULL;
}

media_buffer_t *wav_demux_buffer(wav_demux_t *demux) {
    return demux ? demux->reader : NULL;
}

bool wav_demux_read(wav_demux_t *demux, wav_packet_t *packet) {
    if (!demux || !packet || demux->cursor >= demux->data_end) return false;

    uint32_t units = WAV_PACKET_BYTES / demux->unit_bytes;
    if (!units) units = 1;
    const uint64_t left = (uint64_t)(demux->data_end - demux->cursor) / demux->unit_bytes;
    if (left == 0) return false;
    if (units > left) units = (uint32_t)left;
    const uint32_t size = units * demux->unit_bytes;

    uint32_t ref = MB_NO_REF;
    mb_seek(demux->reader, demux->cursor);
    const uint8_t *data = mb_view(demux->reader, size, &ref);
    if (!data) return false;

    packet->pts_us =
        units_to_us(demux, (uint64_t)(demux->cursor - demux->data_start) / demux->unit_bytes);
    packet->data = data;
    packet->size = size;
    packet->ref = ref;
    demux->cursor += size;
    return true;
}

bool wav_demux_seek(wav_demux_t *demux, int64_t pts_us, int64_t *landed_us) {
    if (!demux) return false;
    uint64_t units = us_to_units(demux, pts_us);
    const uint64_t total = (uint64_t)(demux->data_end - demux->data_start) / demux->unit_bytes;
    if (units > total) units = total;
    demux->cursor = demux->data_start + (off_t)(units * demux->unit_bytes);
    mb_seek(demux->reader, demux->cursor);
    if (landed_us) *landed_us = units_to_us(demux, units);
    return true;
}
