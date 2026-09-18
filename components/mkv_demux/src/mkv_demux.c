/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "mkv_demux.h"

#include <math.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "mkv_demux";

#define ID_EBML              0x1A45DFA3
#define ID_DOC_TYPE          0x4282
#define ID_SEGMENT           0x18538067
#define ID_SEEK_HEAD         0x114D9B74
#define ID_SEEK              0x4DBB
#define ID_SEEK_ID           0x53AB
#define ID_SEEK_POSITION     0x53AC
#define ID_INFO              0x1549A966
#define ID_TIMESTAMP_SCALE   0x2AD7B1
#define ID_DURATION          0x4489
#define ID_TRACKS            0x1654AE6B
#define ID_TRACK_ENTRY       0xAE
#define ID_TRACK_NUMBER      0xD7
#define ID_TRACK_TYPE        0x83
#define ID_CODEC_ID          0x86
#define ID_CODEC_PRIVATE     0x63A2
#define ID_DEFAULT_DURATION  0x23E383
#define ID_CONTENT_ENCODINGS 0x6D80
#define ID_VIDEO             0xE0
#define ID_PIXEL_WIDTH       0xB0
#define ID_PIXEL_HEIGHT      0xBA
#define ID_PROJECTION        0x7670
#define ID_PROJECTION_TYPE   0x7671
#define ID_POSE_YAW          0x7673
#define ID_POSE_PITCH        0x7674
#define ID_POSE_ROLL         0x7675
#define ID_AUDIO             0xE1
#define ID_SAMPLING_FREQ     0xB5
#define ID_CHANNELS          0x9F
#define ID_BIT_DEPTH         0x6264
#define ID_CLUSTER           0x1F43B675
#define ID_TIMESTAMP         0xE7
#define ID_SIMPLE_BLOCK      0xA3
#define ID_BLOCK_GROUP       0xA0
#define ID_BLOCK             0xA1
#define ID_REFERENCE_BLOCK   0xFB
#define ID_CUES              0x1C53BB6B
#define ID_CUE_POINT         0xBB
#define ID_CUE_TIME          0xB3
#define ID_CUE_TRACK_POS     0xB7
#define ID_CUE_TRACK         0xF7
#define ID_CUE_CLUSTER_POS   0xF1

#define TRACK_TYPE_VIDEO 1
#define TRACK_TYPE_AUDIO 2

#define LACING_MASK  0x06
#define LACING_XIPH  0x02
#define LACING_FIXED 0x04
#define LACING_EBML  0x06
#define MKV_MAX_LACES 256

#define MKV_UNKNOWN_SIZE UINT64_MAX
#define MKV_MAX_EBML_HEADER_BYTES 4096
#define MKV_MAX_HEADER_BYTES (256 * 1024)
#define MKV_MAX_CUE_POINT_BYTES 1024
#define MKV_MAX_CUES 60000
#define MKV_PROBE_BLOCKS 64
#define MKV_ROTATION_TOLERANCE_DEG 1.0
#define MKV_POSE_TOLERANCE_DEG 1.0
#define MKV_WAVE_FORMAT_BYTES 18
#define MKV_WAVE_FORMAT_IMA_ADPCM 0x0011

typedef struct {
    uint32_t time_ms;
    uint32_t position;
} cue_t;

typedef struct {
    uint64_t track;
    int64_t pts_us;
    bool keyframe;
    bool laced;
    uint16_t laces;
    off_t end;
    uint32_t payload;
} block_t;

typedef struct {
    uint64_t number;
    uint64_t type;
    char codec[32];
    const uint8_t *codec_private;
    size_t codec_private_size;
    uint64_t default_duration_ns;
    bool encoded;
    uint32_t width;
    uint32_t height;
    uint64_t projection_type;
    double yaw;
    double pitch;
    double roll;
    double sample_rate;
    uint64_t channels;
    uint64_t bits;
} track_t;

struct mkv_demux {
    media_buffer_t *reader;
    media_arena_t arena;
    mkv_info_t info;
    off_t segment_start;
    off_t segment_end;
    off_t first_cluster;
    off_t cues_offset;
    uint64_t timestamp_scale;
    double duration;
    uint64_t video_track;
    uint64_t audio_track;
    uint64_t default_duration_ns;
    uint64_t cluster_time;
    int64_t skip_before_us;
    bool need_keyframe;
    off_t group_end;
    bool group_has_ref;
    cue_t *cues;
    uint32_t cue_count;
    uint8_t *audio_private;
    uint8_t *video_private;
    uint32_t lace_sizes[MKV_MAX_LACES];
    uint16_t lace_count;
    uint16_t lace_next;
    int64_t lace_pts_us;
    off_t lace_end;
};

typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} span_t;

static int vint_length(uint8_t first) {
    for (int length = 1; length <= 8; length++) {
        if (first & (0x80 >> (length - 1))) return length;
    }
    return 0;
}

static bool read_byte(media_buffer_t *reader, uint8_t *out) {
    return mb_read(reader, out, 1) == 1;
}

static bool read_id(media_buffer_t *reader, uint32_t *id) {
    uint8_t byte = 0;
    if (!read_byte(reader, &byte)) return false;
    const int length = vint_length(byte);
    if (length == 0 || length > 4) return false;
    uint32_t value = byte;
    for (int i = 1; i < length; i++) {
        if (!read_byte(reader, &byte)) return false;
        value = (value << 8) | byte;
    }
    *id = value;
    return true;
}

static bool read_vint(media_buffer_t *reader, uint64_t *out) {
    uint8_t byte = 0;
    if (!read_byte(reader, &byte)) return false;
    const int length = vint_length(byte);
    if (length == 0) return false;
    uint64_t value = byte & (0xFF >> length);
    bool all_ones = value == (uint64_t)(0xFF >> length);
    for (int i = 1; i < length; i++) {
        if (!read_byte(reader, &byte)) return false;
        value = (value << 8) | byte;
        all_ones = all_ones && byte == 0xFF;
    }
    *out = all_ones ? MKV_UNKNOWN_SIZE : value;
    return true;
}

static bool read_header(media_buffer_t *reader, uint32_t *id, uint64_t *size) {
    return read_id(reader, id) && read_vint(reader, size);
}

static bool span_vint(span_t *span, uint64_t *out, bool keep_marker) {
    if (span->p >= span->end) return false;
    const int length = vint_length(span->p[0]);
    if (length == 0 || span->p + length > span->end) return false;
    uint64_t value = keep_marker ? span->p[0] : (span->p[0] & (0xFF >> length));
    for (int i = 1; i < length; i++) value = (value << 8) | span->p[i];
    span->p += length;
    *out = value;
    return true;
}

static bool span_next(span_t *span, uint32_t *id, span_t *body) {
    uint64_t raw_id = 0;
    uint64_t size = 0;
    if (!span_vint(span, &raw_id, true) || !span_vint(span, &size, false)) return false;
    if (size > (uint64_t)(span->end - span->p)) return false;
    *id = (uint32_t)raw_id;
    body->p = span->p;
    body->end = span->p + size;
    span->p = body->end;
    return true;
}

static uint64_t span_uint(span_t body) {
    uint64_t value = 0;
    for (const uint8_t *p = body.p; p < body.end && p < body.p + 8; p++) value = (value << 8) | *p;
    return value;
}

static double span_float(span_t body) {
    const uint64_t bits = span_uint(body);
    if (body.end - body.p == 4) {
        const uint32_t narrow = (uint32_t)bits;
        float value = 0;
        memcpy(&value, &narrow, sizeof(value));
        return value;
    }
    if (body.end - body.p == 8) {
        double value = 0;
        memcpy(&value, &bits, sizeof(value));
        return value;
    }
    return 0;
}

static void span_string(span_t body, char *out, size_t capacity) {
    size_t length = (size_t)(body.end - body.p);
    if (length >= capacity) length = capacity - 1;
    memcpy(out, body.p, length);
    out[length] = '\0';
}

static uint8_t *load_body(media_buffer_t *reader, uint64_t size, size_t limit) {
    if (size == MKV_UNKNOWN_SIZE || size > limit) return NULL;
    uint8_t *data = heap_caps_malloc(size ? (size_t)size : 1, MALLOC_CAP_SPIRAM);
    if (!data) data = heap_caps_malloc(size ? (size_t)size : 1, MALLOC_CAP_DEFAULT);
    if (!data) return NULL;
    if (mb_read(reader, data, (size_t)size) != size) {
        heap_caps_free(data);
        return NULL;
    }
    return data;
}

static void parse_seek_head(mkv_demux_t *demux, span_t span) {
    uint32_t id = 0;
    span_t body;
    while (span_next(&span, &id, &body)) {
        if (id != ID_SEEK) continue;
        uint64_t target = 0;
        uint64_t position = 0;
        bool have_position = false;
        uint32_t child = 0;
        span_t value;
        while (span_next(&body, &child, &value)) {
            if (child == ID_SEEK_ID) target = span_uint(value);
            if (child == ID_SEEK_POSITION) {
                position = span_uint(value);
                have_position = true;
            }
        }
        if (target == ID_CUES && have_position && !demux->cues_offset) {
            demux->cues_offset = demux->segment_start + (off_t)position;
        }
    }
}

static void parse_info(mkv_demux_t *demux, span_t span) {
    uint32_t id = 0;
    span_t body;
    while (span_next(&span, &id, &body)) {
        if (id == ID_TIMESTAMP_SCALE) {
            const uint64_t scale = span_uint(body);
            if (scale) demux->timestamp_scale = scale;
        } else if (id == ID_DURATION) {
            demux->duration = span_float(body);
        }
    }
}

static void parse_projection(track_t *track, span_t span) {
    uint32_t id = 0;
    span_t body;
    while (span_next(&span, &id, &body)) {
        switch (id) {
        case ID_PROJECTION_TYPE: track->projection_type = span_uint(body); break;
        case ID_POSE_YAW:        track->yaw = span_float(body); break;
        case ID_POSE_PITCH:      track->pitch = span_float(body); break;
        case ID_POSE_ROLL:       track->roll = span_float(body); break;
        default: break;
        }
    }
}

static void parse_video(track_t *track, span_t span) {
    uint32_t id = 0;
    span_t body;
    while (span_next(&span, &id, &body)) {
        switch (id) {
        case ID_PIXEL_WIDTH:  track->width = (uint32_t)span_uint(body); break;
        case ID_PIXEL_HEIGHT: track->height = (uint32_t)span_uint(body); break;
        case ID_PROJECTION:   parse_projection(track, body); break;
        default: break;
        }
    }
}

static void parse_audio(track_t *track, span_t span) {
    uint32_t id = 0;
    span_t body;
    while (span_next(&span, &id, &body)) {
        switch (id) {
        case ID_SAMPLING_FREQ: track->sample_rate = span_float(body); break;
        case ID_CHANNELS:      track->channels = span_uint(body); break;
        case ID_BIT_DEPTH:     track->bits = span_uint(body); break;
        default: break;
        }
    }
}

static uint16_t rotation_of(const track_t *track) {
    if (track->projection_type != 0 || fabs(track->yaw) > MKV_POSE_TOLERANCE_DEG ||
        fabs(track->pitch) > MKV_POSE_TOLERANCE_DEG) {
        return 0;
    }
    const double quarters = round(track->roll / 90.0);
    if (fabs(track->roll - quarters * 90.0) > MKV_ROTATION_TOLERANCE_DEG) {
        ESP_LOGW(TAG, "ignoring roll %.1f, not a multiple of 90", track->roll);
        return 0;
    }
    const int turns = (((int)quarters % 4) + 4) % 4;
    return (uint16_t)(turns * 90);
}

static uint8_t *copy_private(const uint8_t *data, size_t size) {
    if (!size) return NULL;
    uint8_t *copy = heap_caps_malloc(size, MALLOC_CAP_DEFAULT);
    if (copy) memcpy(copy, data, size);
    return copy;
}

static void use_video_track(mkv_demux_t *demux, const track_t *track) {
    demux->video_track = track->number;
    demux->default_duration_ns = track->default_duration_ns;
    mkv_video_codec_t codec = MKV_VIDEO_CODEC_UNSUPPORTED;
    if (!track->encoded) {
        if (strcmp(track->codec, "V_MJPEG") == 0) {
            codec = MKV_VIDEO_CODEC_MJPEG;
        } else if (strcmp(track->codec, "V_MPEG4/ISO/AVC") == 0) {
            codec = MKV_VIDEO_CODEC_H264;
            demux->video_private = copy_private(track->codec_private, track->codec_private_size);
            if (demux->video_private) {
                demux->info.video.codec_private = demux->video_private;
                demux->info.video.codec_private_size = (uint32_t)track->codec_private_size;
            }
        }
    }
    demux->info.video.codec = codec;
    demux->info.video.width = track->width;
    demux->info.video.height = track->height;
    demux->info.video.rotation_ccw = rotation_of(track);
}

static uint16_t read_le16(const uint8_t *p) {
    return (uint16_t)(p[0] | (p[1] << 8));
}

static uint32_t read_le32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void keep_audio_private(mkv_demux_t *demux, const uint8_t *data, size_t size) {
    demux->audio_private = copy_private(data, size);
    if (!demux->audio_private) return;
    demux->info.audio.codec_private = demux->audio_private;
    demux->info.audio.codec_private_size = (uint32_t)size;
}

static mkv_audio_codec_t use_wave_format(mkv_demux_t *demux, const track_t *track) {
    const uint8_t *wave = track->codec_private;
    if (!wave || track->codec_private_size < MKV_WAVE_FORMAT_BYTES) return MKV_AUDIO_CODEC_UNSUPPORTED;
    if (read_le16(wave) != MKV_WAVE_FORMAT_IMA_ADPCM) return MKV_AUDIO_CODEC_UNSUPPORTED;

    demux->info.audio.channels = (uint8_t)read_le16(wave + 2);
    demux->info.audio.sample_rate = read_le32(wave + 4);
    demux->info.audio.block_align = read_le16(wave + 12);
    demux->info.audio.bits_per_sample = (uint8_t)read_le16(wave + 14);
    size_t extra = read_le16(wave + 16);
    if (extra > track->codec_private_size - MKV_WAVE_FORMAT_BYTES) {
        extra = track->codec_private_size - MKV_WAVE_FORMAT_BYTES;
    }
    keep_audio_private(demux, wave + MKV_WAVE_FORMAT_BYTES, extra);
    return MKV_AUDIO_CODEC_ADPCM_IMA;
}

static const char *audio_codec_name(mkv_audio_codec_t codec) {
    switch (codec) {
    case MKV_AUDIO_CODEC_NONE: return "none";
    case MKV_AUDIO_CODEC_PCM: return "PCM";
    case MKV_AUDIO_CODEC_MP3: return "MP3";
    case MKV_AUDIO_CODEC_ADPCM_IMA: return "IMA ADPCM";
    case MKV_AUDIO_CODEC_AAC: return "AAC";
    case MKV_AUDIO_CODEC_OPUS: return "Opus";
    default: return "unsupported";
    }
}

static void use_audio_track(mkv_demux_t *demux, const track_t *track) {
    demux->audio_track = track->number;
    demux->info.audio.sample_rate = (uint32_t)lround(track->sample_rate);
    demux->info.audio.channels = (uint8_t)track->channels;
    demux->info.audio.bits_per_sample = track->bits ? (uint8_t)track->bits : 16;

    mkv_audio_codec_t codec = MKV_AUDIO_CODEC_UNSUPPORTED;
    if (!track->encoded) {
        if (strcmp(track->codec, "A_PCM/INT/LIT") == 0) {
            codec = MKV_AUDIO_CODEC_PCM;
        } else if (strcmp(track->codec, "A_MPEG/L3") == 0) {
            codec = MKV_AUDIO_CODEC_MP3;
        } else if (strncmp(track->codec, "A_AAC", 5) == 0) {
            codec = MKV_AUDIO_CODEC_AAC;
            keep_audio_private(demux, track->codec_private, track->codec_private_size);
        } else if (strcmp(track->codec, "A_OPUS") == 0) {
            codec = MKV_AUDIO_CODEC_OPUS;
            keep_audio_private(demux, track->codec_private, track->codec_private_size);
        } else if (strcmp(track->codec, "A_MS/ACM") == 0) {
            codec = use_wave_format(demux, track);
        }
    }
    demux->info.audio.codec = codec;
    if (!demux->info.audio.sample_rate || !demux->info.audio.channels) {
        demux->info.audio.codec = MKV_AUDIO_CODEC_UNSUPPORTED;
    }
}

static void parse_tracks(mkv_demux_t *demux, span_t span) {
    uint32_t id = 0;
    span_t entry;
    while (span_next(&span, &id, &entry)) {
        if (id != ID_TRACK_ENTRY) continue;
        track_t track = { .channels = 1 };
        uint32_t child = 0;
        span_t body;
        while (span_next(&entry, &child, &body)) {
            switch (child) {
            case ID_TRACK_NUMBER:      track.number = span_uint(body); break;
            case ID_TRACK_TYPE:        track.type = span_uint(body); break;
            case ID_CODEC_ID:          span_string(body, track.codec, sizeof(track.codec)); break;
            case ID_CODEC_PRIVATE:
                track.codec_private = body.p;
                track.codec_private_size = (size_t)(body.end - body.p);
                break;
            case ID_DEFAULT_DURATION:  track.default_duration_ns = span_uint(body); break;
            case ID_CONTENT_ENCODINGS: track.encoded = true; break;
            case ID_VIDEO:             parse_video(&track, body); break;
            case ID_AUDIO:             parse_audio(&track, body); break;
            default: break;
            }
        }
        if (!track.number) continue;
        if (track.type == TRACK_TYPE_VIDEO && !demux->video_track) use_video_track(demux, &track);
        if (track.type == TRACK_TYPE_AUDIO && !demux->audio_track) use_audio_track(demux, &track);
    }
}

static int64_t to_us(const mkv_demux_t *demux, int64_t timestamp) {
    return timestamp * (int64_t)demux->timestamp_scale / 1000;
}

static bool parse_cue_point(const mkv_demux_t *demux, span_t span, uint64_t *time,
                            uint64_t *position) {
    bool have_time = false;
    bool have_position = false;
    uint32_t id = 0;
    span_t body;
    while (span_next(&span, &id, &body)) {
        if (id == ID_CUE_TIME) {
            *time = span_uint(body);
            have_time = true;
        } else if (id == ID_CUE_TRACK_POS && !have_position) {
            uint64_t track = 0;
            uint64_t cluster = 0;
            bool have_cluster = false;
            uint32_t child = 0;
            span_t value;
            while (span_next(&body, &child, &value)) {
                if (child == ID_CUE_TRACK) track = span_uint(value);
                if (child == ID_CUE_CLUSTER_POS) {
                    cluster = span_uint(value);
                    have_cluster = true;
                }
            }
            if (track == demux->video_track && have_cluster) {
                *position = cluster;
                have_position = true;
            }
        }
    }
    return have_time && have_position;
}

static bool cluster_at(mkv_demux_t *demux, off_t offset) {
    if (offset >= demux->segment_end) return false;
    mb_seek(demux->reader, offset);
    uint32_t id = 0;
    return read_id(demux->reader, &id) && id == ID_CLUSTER;
}

static void load_cues(mkv_demux_t *demux) {
    mb_seek(demux->reader, demux->cues_offset);
    uint32_t id = 0;
    uint64_t size = 0;
    if (!read_header(demux->reader, &id, &size) || id != ID_CUES || size == MKV_UNKNOWN_SIZE) {
        ESP_LOGW(TAG, "no Cues at the SeekHead position; seeking disabled");
        return;
    }

    uint32_t capacity = (uint32_t)(size / 8 + 1);
    if (capacity > MKV_MAX_CUES) capacity = MKV_MAX_CUES;
    cue_t *cues = heap_caps_malloc(capacity * sizeof(cue_t), MALLOC_CAP_SPIRAM);
    if (!cues) cues = heap_caps_malloc(capacity * sizeof(cue_t), MALLOC_CAP_DEFAULT);
    if (!cues) {
        ESP_LOGW(TAG, "no memory for the cue index; seeking disabled");
        return;
    }

    const bool dedup = demux->info.video.codec == MKV_VIDEO_CODEC_MJPEG;
    uint32_t count = 0;
    uint32_t step = 1;
    uint32_t seen = 0;
    uint8_t point[MKV_MAX_CUE_POINT_BYTES];
    const off_t end = mb_tell(demux->reader) + (off_t)size;
    mb_set_readahead(demux->reader, true);
    while (mb_tell(demux->reader) < end) {
        if (!read_header(demux->reader, &id, &size) || size == MKV_UNKNOWN_SIZE) break;
        const off_t next = mb_tell(demux->reader) + (off_t)size;
        if (id != ID_CUE_POINT || size > sizeof(point) ||
            mb_read(demux->reader, point, (size_t)size) != size) {
            mb_seek(demux->reader, next);
            continue;
        }
        uint64_t time = 0;
        uint64_t position = 0;
        const span_t span = { point, point + size };
        if (!parse_cue_point(demux, span, &time, &position) || position > UINT32_MAX) continue;
        if (dedup && count && cues[count - 1].position == (uint32_t)position) continue;
        if (seen++ % step != 0) continue;
        if (count == capacity) {
            for (uint32_t i = 0; i < count / 2; i++) cues[i] = cues[i * 2];
            count /= 2;
            step *= 2;
        }
        cues[count].time_ms = (uint32_t)(to_us(demux, (int64_t)time) / 1000);
        cues[count].position = (uint32_t)position;
        count++;
    }
    mb_set_readahead(demux->reader, false);

    if (!count || !cluster_at(demux, demux->segment_start + (off_t)cues[0].position)) {
        ESP_LOGW(TAG, "Cues do not point at clusters; seeking disabled");
        heap_caps_free(cues);
        return;
    }
    demux->cues = cues;
    demux->cue_count = count;
    demux->info.seekable = true;
}

static bool read_lace_delta(media_buffer_t *reader, int64_t *out) {
    uint8_t byte = 0;
    if (!read_byte(reader, &byte)) return false;
    const int length = vint_length(byte);
    if (length == 0) return false;
    int64_t value = byte & (0xFF >> length);
    for (int i = 1; i < length; i++) {
        if (!read_byte(reader, &byte)) return false;
        value = (value << 8) | byte;
    }
    *out = value - ((INT64_C(1) << (7 * length - 1)) - 1);
    return true;
}

static bool read_lacing(mkv_demux_t *demux, uint8_t lacing, off_t end, uint16_t *laces) {
    media_buffer_t *reader = demux->reader;
    uint32_t *sizes = demux->lace_sizes;
    uint8_t last = 0;
    if (!read_byte(reader, &last)) return false;
    const uint16_t count = (uint16_t)last + 1;

    uint64_t known = 0;
    bool valid = true;
    if (lacing == LACING_XIPH) {
        for (uint16_t i = 0; i < last; i++) {
            uint64_t size = 0;
            uint8_t byte = 0;
            do {
                if (!read_byte(reader, &byte)) return false;
                size += byte;
            } while (byte == 0xFF);
            sizes[i] = (uint32_t)size;
            known += size;
        }
    } else if (lacing == LACING_EBML) {
        uint64_t size = 0;
        if (last && !read_vint(reader, &size)) return false;
        for (uint16_t i = 0; i < last; i++) {
            if (i > 0) {
                int64_t delta = 0;
                if (!read_lace_delta(reader, &delta)) return false;
                const int64_t next = (int64_t)size + delta;
                if (next < 0) valid = false;
                size = next < 0 ? 0 : (uint64_t)next;
            }
            if (size > UINT32_MAX) valid = false;
            sizes[i] = (uint32_t)size;
            known += size;
        }
    }

    const off_t payload = mb_tell(reader);
    if (payload > end) return false;
    const uint64_t total = (uint64_t)(end - payload);
    if (lacing == LACING_FIXED) {
        if (total % count) valid = false;
        for (uint16_t i = 0; i < count; i++) sizes[i] = (uint32_t)(total / count);
    } else if (known > total) {
        valid = false;
    } else {
        sizes[last] = (uint32_t)(total - known);
    }
    *laces = valid ? count : 0;
    return true;
}

static bool group_references(mkv_demux_t *demux, off_t block_end) {
    media_buffer_t *reader = demux->reader;
    mb_seek(reader, block_end);
    while (mb_tell(reader) < demux->group_end) {
        uint32_t id = 0;
        uint64_t size = 0;
        if (!read_header(reader, &id, &size)) return false;
        if (size == MKV_UNKNOWN_SIZE) break;
        if (id == ID_REFERENCE_BLOCK) demux->group_has_ref = true;
        mb_seek(reader, mb_tell(reader) + (off_t)size);
    }
    return true;
}

static bool next_block(mkv_demux_t *demux, block_t *block) {
    media_buffer_t *reader = demux->reader;
    for (;;) {
        if (mb_tell(reader) >= demux->segment_end) return false;
        uint32_t id = 0;
        uint64_t size = 0;
        if (!read_header(reader, &id, &size)) return false;
        if (id == ID_CLUSTER) {
            demux->cluster_time = 0;
            demux->group_end = 0;
            continue;
        }
        if (id == ID_BLOCK_GROUP) {
            demux->group_end = size == MKV_UNKNOWN_SIZE ? 0 : mb_tell(reader) + (off_t)size;
            demux->group_has_ref = false;
            continue;
        }
        if (size == MKV_UNKNOWN_SIZE) return false;

        const off_t end = mb_tell(reader) + (off_t)size;
        if (id == ID_REFERENCE_BLOCK) demux->group_has_ref = true;
        if (id == ID_TIMESTAMP && size <= 8) {
            uint8_t bytes[8];
            if (mb_read(reader, bytes, (size_t)size) != size) return false;
            const span_t span = { bytes, bytes + size };
            demux->cluster_time = span_uint(span);
            continue;
        }
        if (id != ID_SIMPLE_BLOCK && id != ID_BLOCK) {
            mb_seek(reader, end);
            continue;
        }

        uint64_t track = 0;
        uint8_t head[3];
        if (!read_vint(reader, &track) || mb_read(reader, head, sizeof(head)) != sizeof(head)) {
            return false;
        }
        if (track != demux->video_track && track != demux->audio_track) {
            mb_seek(reader, end);
            continue;
        }
        const int16_t relative = (int16_t)((head[0] << 8) | head[1]);
        const uint8_t flags = head[2];
        const uint8_t lacing = flags & LACING_MASK;
        uint16_t laces = 0;
        if (lacing && !read_lacing(demux, lacing, end, &laces)) return false;
        const off_t payload = mb_tell(reader);
        if (payload > end) return false;

        int64_t timestamp = (int64_t)demux->cluster_time + relative;
        if (timestamp < 0) timestamp = 0;
        block->track = track;
        block->pts_us = to_us(demux, timestamp);
        if (id == ID_SIMPLE_BLOCK) {
            block->keyframe = (flags & 0x80) != 0;
        } else if (track == demux->video_track && end <= demux->group_end) {
            if (!group_references(demux, end)) return false;
            mb_seek(reader, payload);
            block->keyframe = !demux->group_has_ref;
        } else {
            block->keyframe = true;
        }
        block->laced = lacing != 0;
        block->laces = laces;
        block->end = end;
        block->payload = (uint32_t)(end - payload);
        return true;
    }
}

static int64_t probe_frame_interval(mkv_demux_t *demux) {
    mb_seek(demux->reader, demux->first_cluster);
    demux->cluster_time = 0;
    int64_t first = -1;
    for (int i = 0; i < MKV_PROBE_BLOCKS; i++) {
        block_t block;
        if (!next_block(demux, &block)) break;
        mb_seek(demux->reader, block.end);
        if (block.track != demux->video_track) continue;
        if (first < 0) {
            first = block.pts_us;
        } else if (block.pts_us > first) {
            return block.pts_us - first;
        }
    }
    return 0;
}

static void log_info(const char *path, const mkv_info_t *info) {
    ESP_LOGI(TAG, "%s: %ux%u, rotation %u, %lld us/frame, %lld us, %s, audio %s %u Hz x%u",
             path, (unsigned)info->video.width, (unsigned)info->video.height,
             (unsigned)info->video.rotation_ccw, (long long)info->video.frame_interval_us,
             (long long)info->duration_us, info->seekable ? "seekable" : "not seekable",
             audio_codec_name(info->audio.codec),
             (unsigned)info->audio.sample_rate, (unsigned)info->audio.channels);
}

static const char *read_ebml_header(mkv_demux_t *demux) {
    uint32_t id = 0;
    uint64_t size = 0;
    if (!read_header(demux->reader, &id, &size) || id != ID_EBML) return "not a Matroska file";
    uint8_t *data = load_body(demux->reader, size, MKV_MAX_EBML_HEADER_BYTES);
    if (!data) return "not a Matroska file";

    char doc_type[16] = "";
    span_t span = { data, data + size };
    span_t body;
    while (span_next(&span, &id, &body)) {
        if (id == ID_DOC_TYPE) span_string(body, doc_type, sizeof(doc_type));
    }
    heap_caps_free(data);
    if (strcmp(doc_type, "matroska") != 0 && strcmp(doc_type, "webm") != 0) {
        return "not a Matroska file";
    }
    return NULL;
}

static const char *read_segment_headers(mkv_demux_t *demux) {
    uint32_t id = 0;
    uint64_t size = 0;
    if (!read_header(demux->reader, &id, &size) || id != ID_SEGMENT) return "no Segment in this MKV";
    const off_t file_end = mb_size(demux->reader);
    demux->segment_start = mb_tell(demux->reader);
    demux->segment_end = file_end;
    if (size != MKV_UNKNOWN_SIZE && demux->segment_start + (off_t)size < file_end) {
        demux->segment_end = demux->segment_start + (off_t)size;
    }

    while (mb_tell(demux->reader) < demux->segment_end) {
        const off_t position = mb_tell(demux->reader);
        if (!read_header(demux->reader, &id, &size)) break;
        if (id == ID_CLUSTER) {
            demux->first_cluster = position;
            return NULL;
        }
        if (size == MKV_UNKNOWN_SIZE) return "unsupported MKV layout";
        const off_t next = mb_tell(demux->reader) + (off_t)size;

        if (id == ID_SEEK_HEAD || id == ID_INFO || id == ID_TRACKS) {
            uint8_t *data = load_body(demux->reader, size, MKV_MAX_HEADER_BYTES);
            if (!data) return "MKV header is too large";
            const span_t span = { data, data + size };
            if (id == ID_SEEK_HEAD) parse_seek_head(demux, span);
            if (id == ID_INFO) parse_info(demux, span);
            if (id == ID_TRACKS) parse_tracks(demux, span);
            heap_caps_free(data);
        } else if (id == ID_CUES && !demux->cues_offset) {
            demux->cues_offset = position;
        }
        mb_seek(demux->reader, next);
    }
    return "no clusters in this MKV";
}

mkv_demux_t *mkv_demux_open(const char *path, const media_arena_t *arena, const char **error) {
    const char *ignored = NULL;
    if (!error) error = &ignored;
    *error = NULL;

    mkv_demux_t *demux = heap_caps_calloc(1, sizeof(*demux), MALLOC_CAP_DEFAULT);
    if (!demux) {
        *error = "out of memory";
        return NULL;
    }
    demux->arena = *arena;
    demux->timestamp_scale = 1000000;
    demux->skip_before_us = -1;

    demux->reader = mb_open(path, arena);
    if (!demux->reader) {
        *error = "cannot open the file";
        heap_caps_free(demux);
        return NULL;
    }

    const char *failure = read_ebml_header(demux);
    if (!failure) failure = read_segment_headers(demux);
    if (!failure && demux->info.video.codec == MKV_VIDEO_CODEC_NONE) failure = "no video track";
    if (!failure && demux->info.video.codec == MKV_VIDEO_CODEC_UNSUPPORTED) {
        failure = "unsupported video codec in this MKV";
    }
    if (failure) {
        *error = failure;
        mkv_demux_close(demux);
        return NULL;
    }

    if (demux->cues_offset) load_cues(demux);

    demux->info.video.frame_interval_us = demux->default_duration_ns
                                              ? (int64_t)(demux->default_duration_ns / 1000)
                                              : probe_frame_interval(demux);
    demux->info.duration_us = (int64_t)(demux->duration * (double)demux->timestamp_scale / 1000.0);
    if (demux->info.duration_us <= 0 && demux->cue_count) {
        demux->info.duration_us = (int64_t)demux->cues[demux->cue_count - 1].time_ms * 1000 +
                                  demux->info.video.frame_interval_us;
    }
    log_info(path, &demux->info);

    mb_seek(demux->reader, demux->first_cluster);
    demux->cluster_time = 0;
    mb_set_readahead(demux->reader, true);
    return demux;
}

void mkv_demux_close(mkv_demux_t *demux) {
    if (!demux) return;
    if (demux->reader) mb_close(demux->reader);
    heap_caps_free(demux->cues);
    heap_caps_free(demux->audio_private);
    heap_caps_free(demux->video_private);
    heap_caps_free(demux);
}

const mkv_info_t *mkv_demux_info(const mkv_demux_t *demux) {
    return demux ? &demux->info : NULL;
}

media_buffer_t *mkv_demux_buffer(mkv_demux_t *demux) {
    return demux ? demux->reader : NULL;
}

static void drop_laces(mkv_demux_t *demux) {
    if (demux->lace_count) mb_seek(demux->reader, demux->lace_end);
    demux->lace_count = 0;
    demux->lace_next = 0;
}

static int read_lace(mkv_demux_t *demux, mkv_packet_t *packet, size_t max_view) {
    const off_t start = mb_tell(demux->reader);
    const uint32_t size = demux->lace_sizes[demux->lace_next];
    if (size == 0 || size > max_view) {
        mb_seek(demux->reader, start + (off_t)size);
        if (++demux->lace_next == demux->lace_count) drop_laces(demux);
        return 0;
    }

    uint32_t ref = MB_NO_REF;
    const uint8_t *data = mb_view(demux->reader, size, &ref);
    if (!data) {
        mb_seek(demux->reader, start);
        return -1;
    }
    mb_seek(demux->reader, start + (off_t)size);
    if (++demux->lace_next == demux->lace_count) drop_laces(demux);

    packet->type = MKV_PACKET_AUDIO;
    packet->pts_us = demux->lace_pts_us;
    packet->keyframe = true;
    packet->data = data;
    packet->size = size;
    packet->ref = ref;
    return 1;
}

bool mkv_demux_read(mkv_demux_t *demux, mkv_packet_t *packet, bool want_audio) {
    if (!demux || !packet) return false;
    const size_t max_view = mb_arena_max_view(&demux->arena);
    const bool audio_usable = demux->info.audio.codec != MKV_AUDIO_CODEC_NONE &&
                              demux->info.audio.codec != MKV_AUDIO_CODEC_UNSUPPORTED;

    for (;;) {
        if (demux->lace_next < demux->lace_count) {
            if (!want_audio) {
                drop_laces(demux);
                continue;
            }
            const int result = read_lace(demux, packet, max_view);
            if (result < 0) return false;
            if (result > 0) return true;
            continue;
        }

        const off_t start = mb_tell(demux->reader);
        const uint64_t cluster_time = demux->cluster_time;
        block_t block;
        if (!next_block(demux, &block)) return false;

        const bool video = block.track == demux->video_track;
        const bool skip = (video ? block.laced : !(want_audio && audio_usable)) ||
                          (block.laced && !block.laces) || block.payload == 0 ||
                          (!block.laced && block.payload > max_view) ||
                          (demux->skip_before_us >= 0 && block.pts_us < demux->skip_before_us) ||
                          (video && demux->need_keyframe && !block.keyframe);
        if (skip) {
            mb_seek(demux->reader, block.end);
            continue;
        }
        if (block.laced) {
            demux->lace_count = block.laces;
            demux->lace_next = 0;
            demux->lace_pts_us = block.pts_us;
            demux->lace_end = block.end;
            continue;
        }

        uint32_t ref = MB_NO_REF;
        const uint8_t *data = mb_view(demux->reader, block.payload, &ref);
        if (!data) {
            mb_seek(demux->reader, start);
            demux->cluster_time = cluster_time;
            return false;
        }
        mb_seek(demux->reader, block.end);
        if (video) {
            demux->skip_before_us = -1;
            demux->need_keyframe = false;
        }

        packet->type = video ? MKV_PACKET_VIDEO : MKV_PACKET_AUDIO;
        packet->pts_us = block.pts_us;
        packet->keyframe = block.keyframe;
        packet->data = data;
        packet->size = block.payload;
        packet->ref = ref;
        return true;
    }
}

static uint32_t cue_at_or_before(const mkv_demux_t *demux, int64_t pts_us) {
    const int64_t target_ms = pts_us / 1000;
    uint32_t low = 0;
    uint32_t high = demux->cue_count;
    while (high - low > 1) {
        const uint32_t middle = low + (high - low) / 2;
        if ((int64_t)demux->cues[middle].time_ms <= target_ms) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return low;
}

bool mkv_demux_seek(mkv_demux_t *demux, int64_t pts_us, int64_t *landed_us) {
    if (!demux) return false;
    if (landed_us) *landed_us = 0;
    if (pts_us <= 0) {
        demux->lace_count = 0;
        demux->lace_next = 0;
        mb_seek(demux->reader, demux->first_cluster);
        demux->cluster_time = 0;
        demux->group_end = 0;
        demux->skip_before_us = -1;
        demux->need_keyframe = false;
        return true;
    }
    if (!demux->cue_count) return false;

    const uint32_t low = cue_at_or_before(demux, pts_us);
    mb_seek(demux->reader, demux->segment_start + (off_t)demux->cues[low].position);
    demux->lace_count = 0;
    demux->lace_next = 0;
    demux->cluster_time = 0;
    demux->group_end = 0;
    if (demux->info.video.codec == MKV_VIDEO_CODEC_MJPEG) {
        demux->skip_before_us = pts_us - demux->info.video.frame_interval_us / 2;
        demux->need_keyframe = false;
        if (landed_us) *landed_us = pts_us;
    } else {
        demux->skip_before_us = (int64_t)demux->cues[low].time_ms * 1000;
        demux->need_keyframe = true;
        if (landed_us) *landed_us = demux->skip_before_us;
    }
    return true;
}

bool mkv_demux_keyframe_before(const mkv_demux_t *demux, int64_t pts_us, int64_t *key_us) {
    if (!demux || !demux->cue_count || demux->info.video.codec == MKV_VIDEO_CODEC_MJPEG) return false;
    const int64_t key = (int64_t)demux->cues[cue_at_or_before(demux, pts_us)].time_ms * 1000;
    if (key > pts_us) return false;
    *key_us = key;
    return true;
}
