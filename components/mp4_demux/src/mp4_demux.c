/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "mp4_demux.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "mp4_demux";

#define FOURCC(a, b, c, d) \
    (((uint32_t)(a) << 24) | ((uint32_t)(b) << 16) | ((uint32_t)(c) << 8) | (uint32_t)(d))

#define BOX_MOOV FOURCC('m', 'o', 'o', 'v')
#define BOX_MOOF FOURCC('m', 'o', 'o', 'f')
#define BOX_MVHD FOURCC('m', 'v', 'h', 'd')
#define BOX_MVEX FOURCC('m', 'v', 'e', 'x')
#define BOX_TRAK FOURCC('t', 'r', 'a', 'k')
#define BOX_TKHD FOURCC('t', 'k', 'h', 'd')
#define BOX_EDTS FOURCC('e', 'd', 't', 's')
#define BOX_ELST FOURCC('e', 'l', 's', 't')
#define BOX_MDIA FOURCC('m', 'd', 'i', 'a')
#define BOX_MDHD FOURCC('m', 'd', 'h', 'd')
#define BOX_HDLR FOURCC('h', 'd', 'l', 'r')
#define BOX_MINF FOURCC('m', 'i', 'n', 'f')
#define BOX_STBL FOURCC('s', 't', 'b', 'l')
#define BOX_STSD FOURCC('s', 't', 's', 'd')
#define BOX_STTS FOURCC('s', 't', 't', 's')
#define BOX_CTTS FOURCC('c', 't', 't', 's')
#define BOX_STSS FOURCC('s', 't', 's', 's')
#define BOX_STSC FOURCC('s', 't', 's', 'c')
#define BOX_STSZ FOURCC('s', 't', 's', 'z')
#define BOX_STZ2 FOURCC('s', 't', 'z', '2')
#define BOX_STCO FOURCC('s', 't', 'c', 'o')
#define BOX_CO64 FOURCC('c', 'o', '6', '4')
#define BOX_AVCC FOURCC('a', 'v', 'c', 'C')
#define BOX_ESDS FOURCC('e', 's', 'd', 's')
#define BOX_WAVE FOURCC('w', 'a', 'v', 'e')
#define BOX_DOPS FOURCC('d', 'O', 'p', 's')

#define HANDLER_VIDEO FOURCC('v', 'i', 'd', 'e')
#define HANDLER_AUDIO FOURCC('s', 'o', 'u', 'n')

#define ENTRY_AVC1 FOURCC('a', 'v', 'c', '1')
#define ENTRY_AVC3 FOURCC('a', 'v', 'c', '3')
#define ENTRY_JPEG FOURCC('j', 'p', 'e', 'g')
#define ENTRY_MJPA FOURCC('m', 'j', 'p', 'a')
#define ENTRY_MP4V FOURCC('m', 'p', '4', 'v')
#define ENTRY_MP4A FOURCC('m', 'p', '4', 'a')
#define ENTRY_MP3  FOURCC('.', 'm', 'p', '3')
#define ENTRY_OPUS FOURCC('O', 'p', 'u', 's')
#define ENTRY_SOWT FOURCC('s', 'o', 'w', 't')

#define OTI_MPEG4_AUDIO     0x40
#define OTI_MPEG2_AAC_FIRST 0x66
#define OTI_MPEG2_AAC_LAST  0x68
#define OTI_MPEG2_MP3       0x69
#define OTI_MPEG1_MP3       0x6B
#define OTI_MJPEG           0x6C

#define DESC_ES              0x03
#define DESC_DECODER_CONFIG  0x04
#define DESC_DECODER_INFO    0x05
#define DECODER_CONFIG_BYTES 13

#define VISUAL_ENTRY_BYTES 78
#define AUDIO_ENTRY_BYTES  28
#define AUDIO_ENTRY_V1_BYTES 44
#define AUDIO_ENTRY_V2_BYTES 64
#define DOPS_BYTES 11
#define OPUS_HEAD_BYTES 19

#define MP4_MAX_MOOV_BYTES (16 * 1024 * 1024)
#define MP4_PCM_PACKET_BYTES (32 * 1024)
#define MP4_ROTATION_TOLERANCE_DEG 1.0

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} span_t;

typedef struct {
    const uint8_t *data;
    uint32_t count;
} table_t;

typedef struct {
    uint32_t handler;
    bool enabled;
    uint32_t timescale;
    uint64_t duration;
    int32_t matrix_a;
    int32_t matrix_b;
    span_t elst;
    span_t stsd;
    table_t stts;
    table_t ctts;
    table_t stss;
    table_t stsc;
    table_t stco;
    bool co64;
    const uint8_t *sizes;
    uint32_t sample_size;
    uint8_t size_bits;
    uint32_t sample_count;
    uint32_t fixed_size;
    int64_t media_time;
    int64_t delay_us;

    int codec;
    uint32_t width;
    uint32_t height;
    uint32_t sample_rate;
    uint8_t channels;
    uint8_t bits;
    span_t codec_private;
    bool opus;
} track_t;

typedef struct {
    bool valid;
    uint32_t sample;
    uint32_t chunk;
    uint32_t chunk_left;
    uint32_t stsc_run;
    uint64_t offset;
    uint32_t stts_run;
    uint32_t stts_left;
    uint64_t dts;
    uint32_t ctts_run;
    uint32_t ctts_left;
    uint32_t sync_next;
} cursor_t;

struct mp4_demux {
    media_buffer_t *reader;
    media_arena_t arena;
    mp4_info_t info;
    uint8_t *moov;
    uint8_t *audio_private;
    uint32_t movie_timescale;
    uint64_t movie_duration;
    bool fragmented;
    bool have_video;
    bool have_audio;
    track_t video;
    track_t audio;
    cursor_t video_cursor;
    cursor_t audio_cursor;
    int64_t skip_before_us;
    bool warned_interleave;
};

static uint16_t be16(const uint8_t *p) {
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p) {
    return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static size_t span_size(span_t span) {
    return (size_t)(span.end - span.p);
}

static bool span_box(span_t *span, uint32_t *type, span_t *body) {
    const size_t left = span_size(*span);
    if (left < 8) return false;
    uint64_t size = be32(span->p);
    size_t header = 8;
    if (size == 1) {
        if (left < 16) return false;
        size = be64(span->p + 8);
        header = 16;
    } else if (size == 0) {
        size = left;
    }
    if (size < header || size > left) return false;
    *type = be32(span->p + 4);
    body->p = span->p + header;
    body->end = span->p + size;
    span->p += size;
    return true;
}

static bool span_find(span_t span, uint32_t type, span_t *body) {
    uint32_t found = 0;
    while (span_box(&span, &found, body)) {
        if (found == type) return true;
    }
    return false;
}

static int64_t ts_to_us(int64_t value, uint32_t timescale) {
    if (!timescale) return 0;
    return value / timescale * 1000000 + value % timescale * 1000000 / timescale;
}

static int64_t us_to_ts(int64_t us, uint32_t timescale) {
    return us / 1000000 * timescale + us % 1000000 * timescale / 1000000;
}

static bool load_counted(span_t body, size_t entry_bytes, table_t *table) {
    if (span_size(body) < 8) return false;
    const uint32_t count = be32(body.p + 4);
    if (count > (span_size(body) - 8) / entry_bytes) return false;
    table->data = body.p + 8;
    table->count = count;
    return true;
}

static void load_sizes(track_t *track, span_t body, bool compact) {
    if (span_size(body) < 12) return;
    const uint8_t bits = compact ? body.p[7] : 32;
    if (bits != 4 && bits != 8 && bits != 16 && bits != 32) return;
    const uint32_t sample_size = compact ? 0 : be32(body.p + 4);
    const uint32_t count = be32(body.p + 8);
    if (!sample_size && ((uint64_t)count * bits + 7) / 8 > span_size(body) - 12) return;
    track->sample_size = sample_size;
    track->size_bits = bits;
    track->sizes = sample_size ? NULL : body.p + 12;
    track->sample_count = count;
}

static void parse_stbl(track_t *track, span_t stbl) {
    uint32_t type = 0;
    span_t body;
    while (span_box(&stbl, &type, &body)) {
        switch (type) {
        case BOX_STSD: track->stsd = body; break;
        case BOX_STTS: load_counted(body, 8, &track->stts); break;
        case BOX_CTTS: load_counted(body, 8, &track->ctts); break;
        case BOX_STSS: load_counted(body, 4, &track->stss); break;
        case BOX_STSC: load_counted(body, 12, &track->stsc); break;
        case BOX_STCO:
            if (load_counted(body, 4, &track->stco)) track->co64 = false;
            break;
        case BOX_CO64:
            if (load_counted(body, 8, &track->stco)) track->co64 = true;
            break;
        case BOX_STSZ: load_sizes(track, body, false); break;
        case BOX_STZ2: load_sizes(track, body, true); break;
        default: break;
        }
    }
}

static void parse_tkhd(track_t *track, span_t body) {
    if (span_size(body) < 4) return;
    track->enabled = (be32(body.p) & 0x000001) != 0;
    const size_t matrix = body.p[0] == 1 ? 52 : 40;
    if (span_size(body) < matrix + 8) return;
    track->matrix_a = (int32_t)be32(body.p + matrix);
    track->matrix_b = (int32_t)be32(body.p + matrix + 4);
}

static void parse_mdhd(track_t *track, span_t body) {
    if (body.p[0] == 1 ? span_size(body) < 32 : span_size(body) < 20) return;
    if (body.p[0] == 1) {
        track->timescale = be32(body.p + 20);
        track->duration = be64(body.p + 24);
    } else {
        track->timescale = be32(body.p + 12);
        track->duration = be32(body.p + 16);
    }
}

static void parse_trak(track_t *track, span_t trak) {
    memset(track, 0, sizeof(*track));
    span_t body;
    if (span_find(trak, BOX_TKHD, &body)) parse_tkhd(track, body);
    span_t edts;
    if (span_find(trak, BOX_EDTS, &edts) && span_find(edts, BOX_ELST, &body)) track->elst = body;

    span_t mdia;
    if (!span_find(trak, BOX_MDIA, &mdia)) return;
    if (span_find(mdia, BOX_MDHD, &body) && span_size(body) >= 4) parse_mdhd(track, body);
    if (span_find(mdia, BOX_HDLR, &body) && span_size(body) >= 12) track->handler = be32(body.p + 8);
    span_t minf;
    span_t stbl;
    if (span_find(mdia, BOX_MINF, &minf) && span_find(minf, BOX_STBL, &stbl)) {
        parse_stbl(track, stbl);
    }
}

static uint32_t stsc_first(const track_t *track, uint32_t run) {
    return be32(track->stsc.data + 12 * run);
}

static uint32_t stsc_spc(const track_t *track, uint32_t run) {
    return be32(track->stsc.data + 12 * run + 4);
}

static bool tables_valid(const track_t *track) {
    if (!track->timescale || !track->sample_count || !track->stts.count || !track->stsc.count ||
        !track->stco.count || (!track->sample_size && !track->sizes)) {
        return false;
    }
    if (stsc_first(track, 0) != 1) return false;
    for (uint32_t run = 0; run < track->stsc.count; run++) {
        const uint32_t first = stsc_first(track, run);
        if (!stsc_spc(track, run) || first - 1 >= track->stco.count) return false;
        if (run > 0 && first <= stsc_first(track, run - 1)) return false;
    }
    return true;
}

static uint32_t sample_size(const track_t *track, uint32_t sample) {
    if (track->fixed_size) return track->fixed_size;
    if (track->sample_size) return track->sample_size;
    const uint8_t *sizes = track->sizes;
    switch (track->size_bits) {
    case 32: return be32(sizes + 4 * (size_t)sample);
    case 16: return be16(sizes + 2 * (size_t)sample);
    case 8:  return sizes[sample];
    default: return (sizes[sample / 2] >> ((sample & 1) ? 0 : 4)) & 0x0F;
    }
}

static uint64_t chunk_offset(const track_t *track, uint32_t chunk) {
    return track->co64 ? be64(track->stco.data + 8 * (size_t)chunk)
                       : be32(track->stco.data + 4 * (size_t)chunk);
}

static uint32_t run_count(const table_t *table, uint32_t run) {
    return be32(table->data + 8 * (size_t)run);
}

static uint32_t run_value(const table_t *table, uint32_t run) {
    return be32(table->data + 8 * (size_t)run + 4);
}

static bool find_run(const table_t *table, uint32_t sample, uint32_t *run, uint32_t *left,
                     uint64_t *sum) {
    uint64_t start = 0;
    uint64_t total = 0;
    for (uint32_t i = 0; i < table->count; i++) {
        const uint32_t count = run_count(table, i);
        const uint32_t value = run_value(table, i);
        if (sample < start + count) {
            *run = i;
            *left = (uint32_t)(start + count - sample);
            if (sum) *sum = total + (sample - start) * value;
            return true;
        }
        start += count;
        total += (uint64_t)count * value;
    }
    return false;
}

static void cursor_seek(const track_t *track, cursor_t *cursor, uint32_t sample) {
    memset(cursor, 0, sizeof(*cursor));
    cursor->sample = sample;
    if (sample >= track->sample_count) return;

    uint64_t base = 0;
    uint32_t index = 0;
    bool found = false;
    for (uint32_t run = 0; run < track->stsc.count && !found; run++) {
        const uint32_t first = stsc_first(track, run) - 1;
        const uint32_t spc = stsc_spc(track, run);
        const uint32_t limit = run + 1 < track->stsc.count ? stsc_first(track, run + 1) - 1
                                                            : track->stco.count;
        const uint64_t chunks = (limit < track->stco.count ? limit : track->stco.count) - first;
        if (sample < base + chunks * spc) {
            const uint64_t within = sample - base;
            cursor->chunk = first + (uint32_t)(within / spc);
            index = (uint32_t)(within % spc);
            cursor->chunk_left = spc - index;
            cursor->stsc_run = run;
            found = true;
        }
        base += chunks * spc;
    }
    if (!found) return;

    cursor->offset = chunk_offset(track, cursor->chunk);
    for (uint32_t i = sample - index; i < sample; i++) cursor->offset += sample_size(track, i);

    if (!find_run(&track->stts, sample, &cursor->stts_run, &cursor->stts_left, &cursor->dts)) {
        return;
    }
    if (track->ctts.data &&
        !find_run(&track->ctts, sample, &cursor->ctts_run, &cursor->ctts_left, NULL)) {
        cursor->ctts_left = 0;
    }

    uint32_t low = 0;
    uint32_t high = track->stss.count;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        if (be32(track->stss.data + 4 * (size_t)middle) < sample + 1) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    cursor->sync_next = low;
    cursor->valid = true;
}

static bool next_run(const table_t *table, uint32_t *run, uint32_t *left) {
    do {
        (*run)++;
    } while (*run < table->count && run_count(table, *run) == 0);
    if (*run >= table->count) return false;
    *left = run_count(table, *run);
    return true;
}

static void cursor_next(const track_t *track, cursor_t *cursor) {
    if (!cursor->valid) return;
    cursor->offset += sample_size(track, cursor->sample);
    cursor->sample++;
    if (cursor->sample >= track->sample_count) {
        cursor->valid = false;
        return;
    }

    if (--cursor->chunk_left == 0) {
        cursor->chunk++;
        if (cursor->chunk >= track->stco.count) {
            cursor->valid = false;
            return;
        }
        if (cursor->stsc_run + 1 < track->stsc.count &&
            stsc_first(track, cursor->stsc_run + 1) - 1 <= cursor->chunk) {
            cursor->stsc_run++;
        }
        cursor->chunk_left = stsc_spc(track, cursor->stsc_run);
        cursor->offset = chunk_offset(track, cursor->chunk);
    }

    cursor->dts += run_value(&track->stts, cursor->stts_run);
    if (--cursor->stts_left == 0 &&
        !next_run(&track->stts, &cursor->stts_run, &cursor->stts_left)) {
        cursor->valid = false;
        return;
    }

    if (cursor->ctts_left && --cursor->ctts_left == 0 &&
        !next_run(&track->ctts, &cursor->ctts_run, &cursor->ctts_left)) {
        cursor->ctts_left = 0;
    }

    while (cursor->sync_next < track->stss.count &&
           be32(track->stss.data + 4 * (size_t)cursor->sync_next) < cursor->sample + 1) {
        cursor->sync_next++;
    }
}

static void cursor_advance(const track_t *track, cursor_t *cursor, uint32_t samples) {
    for (uint32_t i = 0; i < samples && cursor->valid; i++) cursor_next(track, cursor);
}

static int64_t cursor_pts_us(const track_t *track, const cursor_t *cursor) {
    const int64_t offset = cursor->ctts_left
                               ? (int32_t)run_value(&track->ctts, cursor->ctts_run) : 0;
    const int64_t time = (int64_t)cursor->dts + offset - track->media_time;
    return ts_to_us(time, track->timescale) + track->delay_us;
}

static bool cursor_keyframe(const track_t *track, const cursor_t *cursor) {
    if (!track->stss.data) return true;
    return cursor->sync_next < track->stss.count &&
           be32(track->stss.data + 4 * (size_t)cursor->sync_next) == cursor->sample + 1;
}

static uint32_t sample_at_us(const track_t *track, int64_t us, bool after) {
    int64_t target = us_to_ts(us - track->delay_us, track->timescale) + track->media_time;
    if (target < 0) target = 0;

    uint64_t start = 0;
    uint64_t time = 0;
    for (uint32_t run = 0; run < track->stts.count; run++) {
        const uint32_t count = run_count(&track->stts, run);
        const uint32_t delta = run_value(&track->stts, run);
        const uint64_t length = (uint64_t)count * delta;
        if ((uint64_t)target < time + length) {
            const uint64_t within = (uint64_t)target - time;
            uint64_t step = after ? (within + delta - 1) / delta : within / delta;
            if (step > count) step = count;
            uint64_t sample = start + step;
            if (sample >= track->sample_count) break;
            return (uint32_t)sample;
        }
        start += count;
        time += length;
    }
    return after ? track->sample_count : track->sample_count - 1;
}

static uint32_t sync_at_or_before(const track_t *track, uint32_t sample) {
    if (!track->stss.data) return sample;
    uint32_t low = 0;
    uint32_t high = track->stss.count;
    while (low < high) {
        const uint32_t middle = low + (high - low) / 2;
        if (be32(track->stss.data + 4 * (size_t)middle) <= sample + 1) {
            low = middle + 1;
        } else {
            high = middle;
        }
    }
    if (low == 0) return 0;
    const uint32_t sync = be32(track->stss.data + 4 * (size_t)(low - 1));
    return sync ? sync - 1 : 0;
}

static void apply_edits(track_t *track, uint32_t movie_timescale) {
    const span_t elst = track->elst;
    if (!elst.p || span_size(elst) < 8) return;
    const bool wide = elst.p[0] == 1;
    const size_t entry = wide ? 20 : 12;
    const uint32_t count = be32(elst.p + 4);
    if (count > (span_size(elst) - 8) / entry) return;

    bool have_media = false;
    for (uint32_t i = 0; i < count; i++) {
        const uint8_t *p = elst.p + 8 + i * entry;
        const uint64_t duration = wide ? be64(p) : be32(p);
        const int64_t media_time = wide ? (int64_t)be64(p + 8) : (int32_t)be32(p + 4);
        if (media_time == -1) {
            if (!have_media) track->delay_us += ts_to_us((int64_t)duration, movie_timescale);
            continue;
        }
        if (have_media) {
            ESP_LOGW(TAG, "ignoring edits after the first");
            break;
        }
        track->media_time = media_time;
        have_media = true;
    }
}

static bool read_descriptor(span_t *span, uint8_t *tag, span_t *body) {
    if (span->p >= span->end) return false;
    *tag = *span->p++;
    uint32_t length = 0;
    for (int i = 0; i < 4; i++) {
        if (span->p >= span->end) return false;
        const uint8_t byte = *span->p++;
        length = (length << 7) | (byte & 0x7F);
        if (!(byte & 0x80)) break;
    }
    if (length > span_size(*span)) return false;
    body->p = span->p;
    body->end = span->p + length;
    span->p += length;
    return true;
}

static bool parse_esds(span_t esds, uint8_t *object_type, span_t *decoder_info) {
    if (span_size(esds) < 4) return false;
    esds.p += 4;
    uint8_t tag = 0;
    span_t es;
    if (!read_descriptor(&esds, &tag, &es) || tag != DESC_ES || span_size(es) < 3) return false;
    const uint8_t flags = es.p[2];
    es.p += 3;
    if (flags & 0x80) es.p += 2;
    if (flags & 0x40) {
        if (es.p >= es.end) return false;
        es.p += 1 + es.p[0];
    }
    if (flags & 0x20) es.p += 2;
    if (es.p > es.end) return false;

    span_t config;
    while (read_descriptor(&es, &tag, &config)) {
        if (tag != DESC_DECODER_CONFIG) continue;
        if (span_size(config) < DECODER_CONFIG_BYTES) return false;
        *object_type = config.p[0];
        config.p += DECODER_CONFIG_BYTES;
        decoder_info->p = decoder_info->end = NULL;
        span_t info;
        while (read_descriptor(&config, &tag, &info)) {
            if (tag == DESC_DECODER_INFO) {
                *decoder_info = info;
                break;
            }
        }
        return true;
    }
    return false;
}

static bool first_entry(const track_t *track, uint32_t *type, span_t *entry) {
    if (!track->stsd.p || span_size(track->stsd) < 8) return false;
    span_t entries = { track->stsd.p + 8, track->stsd.end };
    return span_box(&entries, type, entry);
}

static void identify_video(track_t *track) {
    track->codec = MP4_VIDEO_CODEC_UNSUPPORTED;
    uint32_t type = 0;
    span_t entry;
    if (!first_entry(track, &type, &entry) || span_size(entry) < VISUAL_ENTRY_BYTES) return;
    track->width = be16(entry.p + 24);
    track->height = be16(entry.p + 26);
    const span_t children = { entry.p + VISUAL_ENTRY_BYTES, entry.end };

    span_t body;
    uint8_t object_type = 0;
    span_t info;
    switch (type) {
    case ENTRY_AVC1:
    case ENTRY_AVC3:
        track->codec = MP4_VIDEO_CODEC_H264;
        if (span_find(children, BOX_AVCC, &body)) track->codec_private = body;
        break;
    case ENTRY_JPEG:
    case ENTRY_MJPA:
        track->codec = MP4_VIDEO_CODEC_MJPEG;
        break;
    case ENTRY_MP4V:
        if (span_find(children, BOX_ESDS, &body) && parse_esds(body, &object_type, &info) &&
            object_type == OTI_MJPEG) {
            track->codec = MP4_VIDEO_CODEC_MJPEG;
        }
        break;
    default:
        break;
    }
}

static bool find_esds(span_t children, span_t *esds) {
    if (span_find(children, BOX_ESDS, esds)) return true;
    span_t wave;
    return span_find(children, BOX_WAVE, &wave) && span_find(wave, BOX_ESDS, esds);
}

static void identify_audio(track_t *track) {
    track->codec = MP4_AUDIO_CODEC_UNSUPPORTED;
    uint32_t type = 0;
    span_t entry;
    if (!first_entry(track, &type, &entry) || span_size(entry) < AUDIO_ENTRY_BYTES) return;

    const uint16_t version = be16(entry.p + 8);
    size_t header = AUDIO_ENTRY_BYTES;
    track->channels = (uint8_t)be16(entry.p + 16);
    track->bits = (uint8_t)be16(entry.p + 18);
    track->sample_rate = be32(entry.p + 24) >> 16;
    if (version == 1) {
        header = AUDIO_ENTRY_V1_BYTES;
    } else if (version == 2) {
        header = AUDIO_ENTRY_V2_BYTES;
        if (span_size(entry) < header) return;
        double rate = 0;
        const uint64_t bits = be64(entry.p + 32);
        memcpy(&rate, &bits, sizeof(rate));
        track->sample_rate = (uint32_t)lround(rate);
        track->channels = (uint8_t)be32(entry.p + 40);
        track->bits = (uint8_t)be32(entry.p + 48);
    }
    if (span_size(entry) < header) return;
    const span_t children = { entry.p + header, entry.end };

    span_t body;
    uint8_t object_type = 0;
    span_t info;
    switch (type) {
    case ENTRY_MP4A:
        if (!find_esds(children, &body) || !parse_esds(body, &object_type, &info)) break;
        if (object_type == OTI_MPEG4_AUDIO ||
            (object_type >= OTI_MPEG2_AAC_FIRST && object_type <= OTI_MPEG2_AAC_LAST)) {
            track->codec = MP4_AUDIO_CODEC_AAC;
            track->codec_private = info;
        } else if (object_type == OTI_MPEG2_MP3 || object_type == OTI_MPEG1_MP3) {
            track->codec = MP4_AUDIO_CODEC_MP3;
        }
        break;
    case ENTRY_MP3:
        track->codec = MP4_AUDIO_CODEC_MP3;
        break;
    case ENTRY_OPUS:
        if (!span_find(children, BOX_DOPS, &body) || span_size(body) < DOPS_BYTES) break;
        track->codec = MP4_AUDIO_CODEC_OPUS;
        track->codec_private = body;
        track->channels = body.p[1];
        track->opus = true;
        break;
    case ENTRY_SOWT:
        if (track->bits != 16 || !track->channels) break;
        track->codec = MP4_AUDIO_CODEC_PCM;
        track->fixed_size = (uint32_t)track->channels * 2;
        break;
    default:
        break;
    }
    if (!track->sample_rate || !track->channels) track->codec = MP4_AUDIO_CODEC_UNSUPPORTED;
}

static int track_score(const track_t *track, int unsupported) {
    if (!tables_valid(track)) return -1;
    return (track->codec != unsupported ? 2 : 0) + (track->enabled ? 1 : 0);
}

static const char *parse_moov(mp4_demux_t *demux, span_t moov) {
    int video_score = -1;
    int audio_score = -1;
    uint32_t type = 0;
    span_t body;
    while (span_box(&moov, &type, &body)) {
        if (type == BOX_MVEX) demux->fragmented = true;
        if (type == BOX_MVHD && span_size(body) >= 4) {
            track_t header;
            memset(&header, 0, sizeof(header));
            parse_mdhd(&header, body);
            demux->movie_timescale = header.timescale;
            demux->movie_duration = header.duration;
        }
        if (type != BOX_TRAK) continue;

        track_t track;
        parse_trak(&track, body);
        if (track.handler == HANDLER_VIDEO) {
            identify_video(&track);
            const int score = track_score(&track, MP4_VIDEO_CODEC_UNSUPPORTED);
            if (score > video_score) {
                video_score = score;
                demux->video = track;
                demux->have_video = true;
            }
        } else if (track.handler == HANDLER_AUDIO) {
            identify_audio(&track);
            const int score = track_score(&track, MP4_AUDIO_CODEC_UNSUPPORTED);
            if (score > audio_score) {
                audio_score = score;
                demux->audio = track;
                demux->have_audio = true;
            }
        }
    }
    if (demux->fragmented) return "fragmented MP4 is not supported";
    if (!demux->have_video) return "no video track";
    if (demux->video.codec == MP4_VIDEO_CODEC_UNSUPPORTED) return "unsupported video codec in this MP4";
    return NULL;
}

static bool printable(uint32_t type) {
    for (int i = 0; i < 4; i++) {
        const uint8_t c = (uint8_t)(type >> (8 * i));
        if (c < 0x20 || c > 0x7E) return false;
    }
    return true;
}

static const char *load_moov(mp4_demux_t *demux, span_t *moov) {
    const off_t file_end = mb_size(demux->reader);
    off_t position = 0;
    bool first = true;
    while (position + 8 <= file_end) {
        uint8_t header[16];
        mb_seek(demux->reader, position);
        if (mb_read(demux->reader, header, 8) != 8) break;
        uint64_t size = be32(header);
        const uint32_t type = be32(header + 4);
        size_t header_bytes = 8;
        if (size == 1) {
            if (mb_read(demux->reader, header + 8, 8) != 8) break;
            size = be64(header + 8);
            header_bytes = 16;
        } else if (size == 0) {
            size = (uint64_t)(file_end - position);
        }
        if (first && (!printable(type) || size < header_bytes)) return "not an MP4 file";
        if (size < header_bytes || size > (uint64_t)(file_end - position)) break;

        if (type == BOX_MOOF) return "fragmented MP4 is not supported";
        if (type == BOX_MOOV) {
            const uint64_t bytes = size - header_bytes;
            if (bytes > MP4_MAX_MOOV_BYTES) return "MP4 index is too large";
            demux->moov = heap_caps_malloc(bytes ? (size_t)bytes : 1, MALLOC_CAP_SPIRAM);
            if (!demux->moov) demux->moov = heap_caps_malloc(bytes ? (size_t)bytes : 1, MALLOC_CAP_DEFAULT);
            if (!demux->moov) return "MP4 index is too large";
            if (mb_read(demux->reader, demux->moov, (size_t)bytes) != bytes) return "cannot read the MP4 index";
            moov->p = demux->moov;
            moov->end = demux->moov + bytes;
            return NULL;
        }
        position += (off_t)size;
        first = false;
    }
    return first ? "not an MP4 file" : "no moov in this MP4";
}

static uint16_t rotation_of(const track_t *track) {
    if (!track->matrix_a && !track->matrix_b) return 0;
    const double degrees = -atan2((double)track->matrix_b, (double)track->matrix_a) * 180.0 / M_PI;
    const double quarters = round(degrees / 90.0);
    if (fabs(degrees - quarters * 90.0) > MP4_ROTATION_TOLERANCE_DEG) {
        ESP_LOGW(TAG, "ignoring rotation %.1f, not a multiple of 90", degrees);
        return 0;
    }
    const int turns = (((int)quarters % 4) + 4) % 4;
    return (uint16_t)(turns * 90);
}

static int64_t frame_interval_us(const track_t *track) {
    uint32_t best = 0;
    for (uint32_t run = 1; run < track->stts.count; run++) {
        if (run_count(&track->stts, run) > run_count(&track->stts, best)) best = run;
    }
    return ts_to_us(run_value(&track->stts, best), track->timescale);
}

static void keep_opus_head(mp4_demux_t *demux, span_t dops) {
    const size_t table = span_size(dops) - DOPS_BYTES;
    uint8_t *head = heap_caps_malloc(OPUS_HEAD_BYTES + table, MALLOC_CAP_DEFAULT);
    if (!head) return;
    const uint8_t *p = dops.p;
    memcpy(head, "OpusHead", 8);
    head[8] = 1;
    head[9] = p[1];
    head[10] = p[3];
    head[11] = p[2];
    head[12] = p[7];
    head[13] = p[6];
    head[14] = p[5];
    head[15] = p[4];
    head[16] = p[9];
    head[17] = p[8];
    head[18] = p[10];
    memcpy(head + OPUS_HEAD_BYTES, p + DOPS_BYTES, table);
    demux->audio_private = head;
    demux->info.audio.codec_private = head;
    demux->info.audio.codec_private_size = (uint32_t)(OPUS_HEAD_BYTES + table);
}

static void fill_info(mp4_demux_t *demux) {
    track_t *video = &demux->video;
    apply_edits(video, demux->movie_timescale);
    demux->info.video.codec = (mp4_video_codec_t)video->codec;
    demux->info.video.width = video->width;
    demux->info.video.height = video->height;
    demux->info.video.rotation_ccw = rotation_of(video);
    demux->info.video.frame_interval_us = frame_interval_us(video);
    if (video->codec_private.p) {
        demux->info.video.codec_private = video->codec_private.p;
        demux->info.video.codec_private_size = (uint32_t)span_size(video->codec_private);
    }

    if (demux->have_audio) {
        track_t *audio = &demux->audio;
        apply_edits(audio, demux->movie_timescale);
        demux->info.audio.codec = (mp4_audio_codec_t)audio->codec;
        demux->info.audio.sample_rate = audio->sample_rate;
        demux->info.audio.channels = audio->channels;
        demux->info.audio.bits_per_sample = audio->bits ? audio->bits : 16;
        if (audio->opus) {
            keep_opus_head(demux, audio->codec_private);
        } else if (audio->codec_private.p) {
            demux->info.audio.codec_private = audio->codec_private.p;
            demux->info.audio.codec_private_size = (uint32_t)span_size(audio->codec_private);
        }
    }

    int64_t duration = 0;
    if (demux->movie_timescale && demux->movie_duration &&
        demux->movie_duration != UINT32_MAX && demux->movie_duration != UINT64_MAX) {
        duration = ts_to_us((int64_t)demux->movie_duration, demux->movie_timescale);
    }
    if (duration <= 0) duration = ts_to_us((int64_t)video->duration, video->timescale);
    demux->info.duration_us = duration;
    demux->info.seekable = true;
}

static const char *audio_codec_name(mp4_audio_codec_t codec) {
    switch (codec) {
    case MP4_AUDIO_CODEC_NONE: return "none";
    case MP4_AUDIO_CODEC_PCM: return "PCM";
    case MP4_AUDIO_CODEC_MP3: return "MP3";
    case MP4_AUDIO_CODEC_AAC: return "AAC";
    case MP4_AUDIO_CODEC_OPUS: return "Opus";
    default: return "unsupported";
    }
}

static void log_info(const char *path, const mp4_demux_t *demux) {
    const mp4_info_t *info = &demux->info;
    ESP_LOGI(TAG, "%s: %ux%u, rotation %u, %lld us/frame, %lld us, %u frames, audio %s %u Hz x%u",
             path, (unsigned)info->video.width, (unsigned)info->video.height,
             (unsigned)info->video.rotation_ccw, (long long)info->video.frame_interval_us,
             (long long)info->duration_us, (unsigned)demux->video.sample_count,
             audio_codec_name(info->audio.codec), (unsigned)info->audio.sample_rate,
             (unsigned)info->audio.channels);
}

mp4_demux_t *mp4_demux_open(const char *path, const media_arena_t *arena, const char **error) {
    const char *ignored = NULL;
    if (!error) error = &ignored;
    *error = NULL;

    mp4_demux_t *demux = heap_caps_calloc(1, sizeof(*demux), MALLOC_CAP_DEFAULT);
    if (!demux) {
        *error = "out of memory";
        return NULL;
    }
    demux->arena = *arena;
    demux->skip_before_us = -1;

    demux->reader = mb_open(path, arena);
    if (!demux->reader) {
        *error = "cannot open the file";
        heap_caps_free(demux);
        return NULL;
    }

    span_t moov = { NULL, NULL };
    const char *failure = load_moov(demux, &moov);
    if (!failure) failure = parse_moov(demux, moov);
    if (failure) {
        *error = failure;
        mp4_demux_close(demux);
        return NULL;
    }

    fill_info(demux);
    cursor_seek(&demux->video, &demux->video_cursor, 0);
    if (demux->have_audio) cursor_seek(&demux->audio, &demux->audio_cursor, 0);
    log_info(path, demux);

    mb_seek(demux->reader, (off_t)demux->video_cursor.offset);
    mb_set_readahead(demux->reader, true);
    return demux;
}

void mp4_demux_close(mp4_demux_t *demux) {
    if (!demux) return;
    if (demux->reader) mb_close(demux->reader);
    heap_caps_free(demux->moov);
    heap_caps_free(demux->audio_private);
    heap_caps_free(demux);
}

const mp4_info_t *mp4_demux_info(const mp4_demux_t *demux) {
    return demux ? &demux->info : NULL;
}

media_buffer_t *mp4_demux_buffer(mp4_demux_t *demux) {
    return demux ? demux->reader : NULL;
}

static bool audio_usable(const mp4_demux_t *demux) {
    return demux->have_audio && demux->info.audio.codec != MP4_AUDIO_CODEC_NONE &&
           demux->info.audio.codec != MP4_AUDIO_CODEC_UNSUPPORTED;
}

static void warn_interleave(mp4_demux_t *demux) {
    if (demux->warned_interleave) return;
    const uint64_t a = demux->video_cursor.offset;
    const uint64_t b = demux->audio_cursor.offset;
    const uint64_t distance = a > b ? a - b : b - a;
    if (distance <= demux->arena.size * 3 / 8) return;
    ESP_LOGW(TAG, "audio and video are %llu bytes apart; reading will be slow",
             (unsigned long long)distance);
    demux->warned_interleave = true;
}

bool mp4_demux_read(mp4_demux_t *demux, mp4_packet_t *packet, bool want_audio) {
    if (!demux || !packet) return false;
    const size_t max_view = mb_arena_max_view(&demux->arena);
    const uint64_t file_size = (uint64_t)mb_size(demux->reader);

    for (;;) {
        cursor_t *const video_cursor = &demux->video_cursor;
        cursor_t *const audio_cursor = &demux->audio_cursor;
        const bool audio = want_audio && audio_usable(demux) && audio_cursor->valid;
        if (!video_cursor->valid && !audio) return false;

        const int64_t video_pts = video_cursor->valid
                                      ? cursor_pts_us(&demux->video, video_cursor) : 0;
        const int64_t audio_pts = audio ? cursor_pts_us(&demux->audio, audio_cursor) : 0;
        const bool is_video = video_cursor->valid && (!audio || video_pts <= audio_pts);
        const track_t *track = is_video ? &demux->video : &demux->audio;
        cursor_t *cursor = is_video ? video_cursor : audio_cursor;

        uint32_t samples = 1;
        uint64_t size = sample_size(track, cursor->sample);
        if (!is_video && track->fixed_size) {
            samples = cursor->chunk_left;
            const uint32_t limit = MP4_PCM_PACKET_BYTES / track->fixed_size;
            if (samples > limit) samples = limit;
            if (samples > track->sample_count - cursor->sample) {
                samples = track->sample_count - cursor->sample;
            }
            size = (uint64_t)samples * track->fixed_size;
        }
        const uint64_t offset = cursor->offset;
        const int64_t pts = is_video ? video_pts : audio_pts;

        if (size == 0 || size > max_view || offset + size > file_size ||
            (is_video && demux->skip_before_us >= 0 && pts < demux->skip_before_us)) {
            cursor_advance(track, cursor, samples);
            continue;
        }

        uint64_t floor = offset;
        if (video_cursor->valid && video_cursor->offset < floor) floor = video_cursor->offset;
        if (audio && audio_cursor->offset < floor) floor = audio_cursor->offset;
        if ((off_t)floor > mb_tell(demux->reader)) mb_seek(demux->reader, (off_t)floor);
        if (video_cursor->valid && audio) warn_interleave(demux);

        uint32_t ref = MB_NO_REF;
        const uint8_t *data = mb_view_at(demux->reader, (off_t)offset, (size_t)size, &ref);
        if (!data) return false;

        const bool keyframe = is_video ? cursor_keyframe(track, cursor) : true;
        cursor_advance(track, cursor, samples);
        if (is_video) demux->skip_before_us = -1;

        packet->type = is_video ? MP4_PACKET_VIDEO : MP4_PACKET_AUDIO;
        packet->pts_us = pts;
        packet->keyframe = keyframe;
        packet->data = data;
        packet->size = (uint32_t)size;
        packet->ref = ref;
        return true;
    }
}

bool mp4_demux_seek(mp4_demux_t *demux, int64_t pts_us, int64_t *landed_us) {
    if (!demux) return false;
    if (landed_us) *landed_us = 0;
    demux->skip_before_us = -1;
    const track_t *video = &demux->video;

    int64_t landed = 0;
    if (pts_us <= 0) {
        cursor_seek(video, &demux->video_cursor, 0);
    } else if (video->codec == MP4_VIDEO_CODEC_H264) {
        const uint32_t sample = sync_at_or_before(video, sample_at_us(video, pts_us, false));
        cursor_seek(video, &demux->video_cursor, sample);
        if (!demux->video_cursor.valid) return false;
        landed = cursor_pts_us(video, &demux->video_cursor);
        if (landed < 0) landed = 0;
    } else {
        const int64_t interval = demux->info.video.frame_interval_us;
        const uint32_t sample = sample_at_us(video, pts_us - interval / 2, true);
        cursor_seek(video, &demux->video_cursor, sample);
        if (!demux->video_cursor.valid) return false;
        landed = pts_us;
    }

    if (demux->have_audio) {
        const uint32_t sample = landed > 0 ? sample_at_us(&demux->audio, landed, true) : 0;
        cursor_seek(&demux->audio, &demux->audio_cursor, sample);
    }
    if (landed_us) *landed_us = landed;
    return true;
}
