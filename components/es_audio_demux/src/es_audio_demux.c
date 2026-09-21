/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "es_audio_demux.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "es_audio_demux";

#define ES_HEADER_BYTES 8
#define ES_SCAN_BYTES 4096
#define ES_SCAN_LIMIT (256 * 1024)
#define ES_VERIFY_FRAMES 3
#define ES_ESTIMATE_FRAMES 64
#define ES_TOC_ENTRIES 100

typedef struct {
    uint32_t size;
    uint32_t samples;
    uint32_t sample_rate;
    uint8_t channels;
} frame_t;

struct es_audio_demux {
    media_buffer_t *reader;
    es_audio_info_t info;
    off_t data_start;
    off_t data_end;
    off_t cursor;
    uint64_t samples;
    bool have_toc;
    uint8_t toc[ES_TOC_ENTRIES];
    /* Off the stack: every task that opens one of these files would otherwise
       carry 4 KiB it needs nowhere else. */
    uint8_t scan[ES_SCAN_BYTES];
};

static uint32_t be32(const uint8_t *p) {
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

static bool parse_mpeg(const uint8_t *h, frame_t *frame) {
    static const uint16_t kBitrateV1[15] = { 0,   32,  40,  48,  56,  64,  80, 96,
                                             112, 128, 160, 192, 224, 256, 320 };
    static const uint16_t kBitrateV2[15] = { 0,  8,  16, 24, 32,  40,  48,  56,
                                             64, 80, 96, 112, 128, 144, 160 };
    static const uint32_t kRates[4][3] = {
        { 11025, 12000, 8000 },
        { 0, 0, 0 },
        { 22050, 24000, 16000 },
        { 44100, 48000, 32000 },
    };

    if (h[0] != 0xFF || (h[1] & 0xE0) != 0xE0) return false;
    const uint8_t version = (h[1] >> 3) & 3;
    const uint8_t layer = (h[1] >> 1) & 3;
    if (version == 1 || layer != 1) return false;

    const uint8_t bitrate_index = h[2] >> 4;
    const uint8_t rate_index = (h[2] >> 2) & 3;
    if (bitrate_index == 0 || bitrate_index == 15 || rate_index == 3) return false;

    const bool version1 = version == 3;
    const uint32_t bitrate =
        (version1 ? kBitrateV1[bitrate_index] : kBitrateV2[bitrate_index]) * 1000u;
    const uint32_t rate = kRates[version][rate_index];
    if (!rate) return false;

    const uint32_t padding = (h[2] >> 1) & 1;
    frame->samples = version1 ? 1152 : 576;
    frame->sample_rate = rate;
    frame->channels = ((h[3] >> 6) & 3) == 3 ? 1 : 2;
    frame->size = (version1 ? 144u : 72u) * bitrate / rate + padding;
    return frame->size > 4;
}

static bool parse_adts(const uint8_t *h, frame_t *frame) {
    static const uint32_t kRates[13] = { 96000, 88200, 64000, 48000, 44100, 32000, 24000,
                                         22050, 16000, 12000, 11025, 8000,  7350 };

    if (h[0] != 0xFF || (h[1] & 0xF6) != 0xF0) return false;
    const uint8_t rate_index = (h[2] >> 2) & 0x0F;
    if (rate_index >= 13) return false;
    const uint8_t channel_config = (uint8_t)(((h[2] & 1) << 2) | (h[3] >> 6));
    if (channel_config == 0 || channel_config > 2) return false;

    const uint32_t length = (uint32_t)((h[3] & 3) << 11 | h[4] << 3 | h[5] >> 5);
    const uint8_t protection_absent = h[1] & 1;
    if (length < (protection_absent ? 7u : 9u)) return false;

    frame->size = length;
    frame->samples = 1024u * (uint32_t)((h[6] & 3) + 1);
    frame->sample_rate = kRates[rate_index];
    frame->channels = channel_config;
    return true;
}

static bool parse_header(const es_audio_demux_t *demux, const uint8_t *h, frame_t *frame) {
    return demux->info.codec == ES_AUDIO_CODEC_AAC ? parse_adts(h, frame) : parse_mpeg(h, frame);
}

static bool read_header(es_audio_demux_t *demux, off_t offset, uint8_t *header) {
    mb_seek(demux->reader, offset);
    return mb_read(demux->reader, header, ES_HEADER_BYTES) == ES_HEADER_BYTES;
}

static bool frames_chain(es_audio_demux_t *demux, off_t offset, frame_t *first) {
    uint8_t header[ES_HEADER_BYTES];
    for (int i = 0; i < ES_VERIFY_FRAMES; i++) {
        frame_t frame;
        if (!read_header(demux, offset, header) || !parse_header(demux, header, &frame)) {
            return false;
        }
        if (i == 0) *first = frame;
        offset += frame.size;
        if (offset > demux->data_end) return false;
        if (offset == demux->data_end) break;
    }
    return true;
}

static bool resync(es_audio_demux_t *demux, off_t from, off_t *found, frame_t *frame) {
    uint8_t *buffer = demux->scan;
    off_t scanned = 0;
    while (from < demux->data_end && scanned < ES_SCAN_LIMIT) {
        mb_seek(demux->reader, from);
        const size_t want = (size_t)(demux->data_end - from) < ES_SCAN_BYTES
                                ? (size_t)(demux->data_end - from) : ES_SCAN_BYTES;
        const size_t got = mb_read(demux->reader, buffer, want);
        if (got < ES_HEADER_BYTES) return false;
        for (size_t i = 0; i + ES_HEADER_BYTES <= got; i++) {
            if (buffer[i] != 0xFF) continue;
            if (frames_chain(demux, from + (off_t)i, frame)) {
                *found = from + (off_t)i;
                return true;
            }
        }
        from += (off_t)(got - (ES_HEADER_BYTES - 1));
        scanned += (off_t)got;
    }
    return false;
}

static off_t read_id3v2(es_audio_demux_t *demux, bool want_cover) {
    off_t start = 0;
    media_tags_read_id3v2(demux->reader, 0, &start, &demux->info.tags, want_cover);
    return start;
}

static off_t trim_tail(es_audio_demux_t *demux, off_t end) {
    uint8_t tail[32];
    if (end >= 128) {
        mb_seek(demux->reader, end - 128);
        if (mb_read(demux->reader, tail, 3) == 3 && memcmp(tail, "TAG", 3) == 0) end -= 128;
    }
    if (end >= 32) {
        mb_seek(demux->reader, end - 32);
        if (mb_read(demux->reader, tail, sizeof(tail)) == sizeof(tail) &&
            memcmp(tail, "APETAGEX", 8) == 0) {
            const uint32_t size = (uint32_t)tail[12] | (uint32_t)tail[13] << 8 |
                                  (uint32_t)tail[14] << 16 | (uint32_t)tail[15] << 24;
            if ((off_t)size <= end) end -= size;
            if ((tail[23] & 0x80) && end >= 32) end -= 32;
        }
    }
    return end;
}

static bool read_vbr_header(es_audio_demux_t *demux, off_t offset, const frame_t *frame,
                            uint64_t *frames) {
    uint8_t buffer[192];
    const uint32_t want = frame->size < sizeof(buffer) ? frame->size : (uint32_t)sizeof(buffer);
    mb_seek(demux->reader, offset);
    if (mb_read(demux->reader, buffer, want) != want) return false;

    for (uint32_t i = 4; i + 12 <= want; i++) {
        const bool xing = memcmp(buffer + i, "Xing", 4) == 0 || memcmp(buffer + i, "Info", 4) == 0;
        if (!xing && memcmp(buffer + i, "VBRI", 4) != 0) continue;
        if (!xing) {
            if (i + 20 > want) return false;
            *frames = be32(buffer + i + 14);
            return *frames > 0;
        }
        const uint32_t flags = be32(buffer + i + 4);
        uint32_t position = i + 8;
        if (!(flags & 1) || position + 4 > want) return false;
        *frames = be32(buffer + position);
        position += 4;
        if (flags & 2) position += 4;
        if ((flags & 4) && position + ES_TOC_ENTRIES <= want) {
            memcpy(demux->toc, buffer + position, ES_TOC_ENTRIES);
            demux->have_toc = true;
        }
        return *frames > 0;
    }
    return false;
}

static int64_t estimate_duration_us(es_audio_demux_t *demux, off_t offset, off_t bytes) {
    uint8_t header[ES_HEADER_BYTES];
    uint64_t frame_bytes = 0;
    uint64_t samples = 0;
    uint32_t rate = 0;
    for (int i = 0; i < ES_ESTIMATE_FRAMES && offset + ES_HEADER_BYTES <= demux->data_end; i++) {
        frame_t frame;
        if (!read_header(demux, offset, header) || !parse_header(demux, header, &frame)) break;
        frame_bytes += frame.size;
        samples += frame.samples;
        rate = frame.sample_rate;
        offset += frame.size;
    }
    if (!frame_bytes || !rate) return 0;
    return (int64_t)((uint64_t)bytes * samples * 1000000ull / (frame_bytes * rate));
}

static off_t byte_for_us(const es_audio_demux_t *demux, int64_t pts_us) {
    const off_t bytes = demux->data_end - demux->data_start;
    if (demux->info.duration_us <= 0 || bytes <= 0) return demux->data_start;
    double ratio = (double)pts_us / (double)demux->info.duration_us;
    if (ratio < 0) ratio = 0;
    if (ratio > 1) ratio = 1;

    if (demux->have_toc) {
        const double percent = ratio * 100.0;
        const int index = (int)percent;
        const double low = demux->toc[index < ES_TOC_ENTRIES ? index : ES_TOC_ENTRIES - 1];
        const double high = index + 1 < ES_TOC_ENTRIES ? demux->toc[index + 1] : 256.0;
        ratio = (low + (high - low) * (percent - index)) / 256.0;
    }
    return demux->data_start + (off_t)(ratio * (double)bytes);
}

static int64_t us_for_byte(const es_audio_demux_t *demux, off_t offset) {
    const off_t bytes = demux->data_end - demux->data_start;
    if (bytes <= 0) return 0;
    const double ratio = (double)(offset - demux->data_start) / (double)bytes;
    return (int64_t)(ratio * (double)demux->info.duration_us);
}

es_audio_demux_t *es_audio_demux_open(const char *path, const media_arena_t *arena,
                                      bool want_cover, const char **error) {
    const char *ignored = NULL;
    if (!error) error = &ignored;
    *error = NULL;

    es_audio_demux_t *demux = heap_caps_calloc(1, sizeof(*demux), MALLOC_CAP_SPIRAM);
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

    demux->info.tags.cover_scanned = true;
    demux->data_start = read_id3v2(demux, want_cover);
    demux->data_end = trim_tail(demux, mb_size(demux->reader));
    media_tags_read_id3v1(demux->reader, mb_size(demux->reader), &demux->info.tags);
    if (demux->data_end <= demux->data_start) {
        *error = "this file carries no audio";
        es_audio_demux_close(demux);
        return NULL;
    }

    off_t first = 0;
    frame_t frame;
    memset(&frame, 0, sizeof(frame));
    for (int attempt = 0; attempt < 2; attempt++) {
        demux->info.codec = attempt == 0 ? ES_AUDIO_CODEC_MP3 : ES_AUDIO_CODEC_AAC;
        if (resync(demux, demux->data_start, &first, &frame)) break;
        demux->info.codec = ES_AUDIO_CODEC_NONE;
    }
    if (demux->info.codec == ES_AUDIO_CODEC_NONE) {
        *error = "no MP3 or AAC frames in this file";
        es_audio_demux_close(demux);
        return NULL;
    }

    demux->data_start = first;
    demux->info.sample_rate = frame.sample_rate;
    demux->info.channels = frame.channels;
    demux->cursor = first;

    const off_t bytes = demux->data_end - demux->data_start;
    uint64_t frames = 0;
    if (demux->info.codec == ES_AUDIO_CODEC_MP3 && read_vbr_header(demux, first, &frame, &frames)) {
        demux->info.duration_us =
            (int64_t)(frames * frame.samples * 1000000ull / frame.sample_rate);
        demux->info.duration_exact = true;
        demux->cursor = first + frame.size;
        demux->data_start = demux->cursor;
    } else {
        demux->info.duration_us = estimate_duration_us(demux, first, bytes);
    }
    demux->info.bitrate_bps =
        demux->info.duration_us > 0
            ? (uint32_t)((uint64_t)(demux->data_end - demux->data_start) * 8 * 1000000ull /
                         (uint64_t)demux->info.duration_us)
            : 0;

    ESP_LOGI(TAG, "%s: %s %u Hz x%u, %lld us%s", path,
             demux->info.codec == ES_AUDIO_CODEC_AAC ? "AAC" : "MP3",
             (unsigned)demux->info.sample_rate, (unsigned)demux->info.channels,
             (long long)demux->info.duration_us, demux->info.duration_exact ? "" : " (estimated)");

    mb_seek(demux->reader, demux->cursor);
    mb_set_readahead(demux->reader, true);
    return demux;
}

void es_audio_demux_close(es_audio_demux_t *demux) {
    if (!demux) return;
    media_tags_free(&demux->info.tags);
    if (demux->reader) mb_close(demux->reader);
    heap_caps_free(demux);
}

const es_audio_info_t *es_audio_demux_info(const es_audio_demux_t *demux) {
    return demux ? &demux->info : NULL;
}

media_buffer_t *es_audio_demux_buffer(es_audio_demux_t *demux) {
    return demux ? demux->reader : NULL;
}

bool es_audio_demux_read(es_audio_demux_t *demux, es_audio_packet_t *packet) {
    if (!demux || !packet) return false;

    uint8_t header[ES_HEADER_BYTES];
    frame_t frame;
    while (demux->cursor + ES_HEADER_BYTES <= demux->data_end) {
        if (!read_header(demux, demux->cursor, header) ||
            !parse_header(demux, header, &frame) ||
            demux->cursor + (off_t)frame.size > demux->data_end) {
            off_t found = 0;
            if (!resync(demux, demux->cursor + 1, &found, &frame)) return false;
            demux->cursor = found;
            continue;
        }

        uint32_t ref = MB_NO_REF;
        mb_seek(demux->reader, demux->cursor);
        const uint8_t *data = mb_view(demux->reader, frame.size, &ref);
        if (!data) return false;

        packet->pts_us = (int64_t)(demux->samples * 1000000ull / demux->info.sample_rate);
        packet->data = data;
        packet->size = frame.size;
        packet->ref = ref;
        demux->cursor += frame.size;
        demux->samples += frame.samples;
        return true;
    }
    return false;
}

bool es_audio_demux_seek(es_audio_demux_t *demux, int64_t pts_us, int64_t *landed_us) {
    if (!demux) return false;

    off_t found = 0;
    frame_t frame;
    if (pts_us <= 0) {
        found = demux->data_start;
        if (!frames_chain(demux, found, &frame) && !resync(demux, found, &found, &frame)) {
            return false;
        }
    } else if (!resync(demux, byte_for_us(demux, pts_us), &found, &frame)) {
        return false;
    }

    const int64_t landed = us_for_byte(demux, found);
    demux->cursor = found;
    demux->samples = (uint64_t)landed * demux->info.sample_rate / 1000000ull;
    mb_seek(demux->reader, demux->cursor);
    if (landed_us) *landed_us = landed;
    return true;
}
