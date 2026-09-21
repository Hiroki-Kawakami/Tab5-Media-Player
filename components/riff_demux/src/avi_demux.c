/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "avi_demux.h"

#include <stdlib.h>
#include <string.h>

#include "riff.h"
#include "media_buffer.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "avi_demux";

#define AVI_MAX_STREAMS 8
#define AVI_MAX_INDEX_ENTRIES 60000

enum {
    STREAM_NONE = 0,
    STREAM_VIDEO,
    STREAM_AUDIO,
};

typedef struct {
    uint32_t frame;
    uint32_t offset;
} seek_point_t;

struct avi_demux {
    media_buffer_t *reader;
    media_arena_t arena;
    avi_info_t info;
    off_t movi_start;
    off_t movi_end;
    off_t idx1_offset;
    uint32_t idx1_size;
    off_t index_base;
    seek_point_t *points;
    uint32_t point_count;
    uint8_t *key_flags;
    uint32_t key_flag_count;
    uint32_t next_frame;
    uint8_t stream_kind[AVI_MAX_STREAMS];
    uint8_t stream_count;
    uint8_t nal_length_size;
    uint8_t *audio_extra;
    uint8_t *video_extra;
};

static int stream_of(const avi_demux_t *demux, uint32_t fourcc, uint8_t *kind) {
    const uint8_t *id = (const uint8_t *)&fourcc;
    if (id[0] < '0' || id[0] > '9' || id[1] < '0' || id[1] > '9') return -1;
    const int index = (id[0] - '0') * 10 + (id[1] - '0');
    if (index >= demux->stream_count) return -1;

    const bool video_suffix = (id[2] == 'd' && (id[3] == 'b' || id[3] == 'c'));
    const bool audio_suffix = (id[2] == 'w' && id[3] == 'b');
    const uint8_t declared = demux->stream_kind[index];
    if (declared == STREAM_VIDEO && video_suffix) {
        *kind = STREAM_VIDEO;
        return index;
    }
    if (declared == STREAM_AUDIO && audio_suffix) {
        *kind = STREAM_AUDIO;
        return index;
    }
    return -1;
}

static avi_video_codec_t video_codec_of(uint32_t compression) {
    switch (compression) {
    case AVI_MJPG:
    case AVI_mjpg:
    case AVI_jpeg:
    case AVI_JPEG:
        return AVI_VIDEO_CODEC_MJPEG;
    case AVI_H264:
    case AVI_h264:
    case AVI_X264:
    case AVI_x264:
    case AVI_AVC1:
    case AVI_avc1:
    case AVI_DAVC:
        return AVI_VIDEO_CODEC_H264;
    case AVI_mpg2:
    case AVI_MPG2:
    case AVI_MPEG:
        return AVI_VIDEO_CODEC_MPEG2;
    default:
        return AVI_VIDEO_CODEC_UNSUPPORTED;
    }
}

static const char *video_codec_name(avi_video_codec_t codec) {
    switch (codec) {
    case AVI_VIDEO_CODEC_MJPEG: return "MJPEG";
    case AVI_VIDEO_CODEC_H264: return "H.264";
    case AVI_VIDEO_CODEC_MPEG2: return "MPEG-2";
    default: return "unsupported";
    }
}

static void read_bitmap_extra(avi_demux_t *demux, uint32_t available) {
    heap_caps_free(demux->video_extra);
    demux->video_extra = NULL;
    demux->info.video.codec_private = NULL;
    demux->info.video.codec_private_size = 0;
    demux->nal_length_size = 0;
    if (available == 0 || available > RIFF_BITMAP_EXTRA_LIMIT) return;

    uint8_t *extra = heap_caps_malloc(available, MALLOC_CAP_DEFAULT);
    if (!extra) return;
    if (!riff_read(demux->reader, extra, available)) {
        heap_caps_free(extra);
        return;
    }
    demux->video_extra = extra;
    demux->info.video.codec_private = extra;
    demux->info.video.codec_private_size = available;
    if (available >= 7 && extra[0] == 1 && (extra[4] & 0x03) != 2) {
        demux->nal_length_size = (uint8_t)((extra[4] & 0x03) + 1);
    }
}

static void parse_stream(avi_demux_t *demux, off_t list_end) {
    avi_stream_header_t strh;
    memset(&strh, 0, sizeof(strh));
    bool have_header = false;
    const int index = demux->stream_count < AVI_MAX_STREAMS ? demux->stream_count : -1;

    while (mb_tell(demux->reader) + (off_t)sizeof(riff_chunk_t) <= list_end) {
        riff_chunk_t chunk;
        if (!riff_read(demux->reader, &chunk, sizeof(chunk))) return;
        const off_t body = mb_tell(demux->reader);
        const off_t next = body + chunk.size + (chunk.size & 1);

        if (chunk.fourcc == AVI_strh && chunk.size >= sizeof(strh)) {
            if (!riff_read(demux->reader, &strh, sizeof(strh))) return;
            have_header = true;
        } else if (chunk.fourcc == AVI_strf && have_header && index >= 0) {
            if (strh.fourcc_type == AVI_vids && chunk.size >= sizeof(avi_bitmap_info_t)) {
                avi_bitmap_info_t bitmap;
                if (!riff_read(demux->reader, &bitmap, sizeof(bitmap))) return;
                demux->stream_kind[index] = STREAM_VIDEO;
                demux->info.video.codec = video_codec_of(bitmap.compression);
                if (bitmap.width) demux->info.video.width = bitmap.width;
                if (bitmap.height) demux->info.video.height = bitmap.height;
                if (strh.rate && strh.scale) {
                    demux->info.video.frame_interval_us =
                        (uint32_t)((uint64_t)strh.scale * 1000000ull / strh.rate);
                }
                if (strh.length) demux->info.video.frame_count = strh.length;
                if (demux->info.video.codec == AVI_VIDEO_CODEC_H264 ||
                    demux->info.video.codec == AVI_VIDEO_CODEC_MPEG2) {
                    uint32_t extra = chunk.size - sizeof(bitmap);
                    if (bitmap.size > sizeof(bitmap) && bitmap.size - sizeof(bitmap) < extra) {
                        extra = bitmap.size - sizeof(bitmap);
                    }
                    read_bitmap_extra(demux, extra);
                }
            } else if (strh.fourcc_type == AVI_auds) {
                riff_audio_format_t format;
                if (!riff_read_wave_format(demux->reader, chunk.size, &format)) return;
                demux->stream_kind[index] = STREAM_AUDIO;
                demux->info.audio.codec = format.codec;
                demux->info.audio.channels = format.channels;
                demux->info.audio.sample_rate = format.sample_rate;
                demux->info.audio.bitrate_bps = format.bitrate_bps;
                demux->info.audio.bits_per_sample = format.bits_per_sample;
                demux->info.audio.block_align = format.block_align;
                heap_caps_free(demux->audio_extra);
                demux->audio_extra = format.extra;
                demux->info.audio.codec_private = format.extra;
                demux->info.audio.codec_private_size = format.extra_size;
            }
        }
        mb_seek(demux->reader, next);
    }

    if (index >= 0) demux->stream_count++;
}

static void parse_header_list(avi_demux_t *demux, off_t list_end) {
    while (mb_tell(demux->reader) + (off_t)sizeof(riff_chunk_t) <= list_end) {
        riff_chunk_t chunk;
        if (!riff_read(demux->reader, &chunk, sizeof(chunk))) return;
        const off_t body = mb_tell(demux->reader);
        const off_t next = body + chunk.size + (chunk.size & 1);

        if (chunk.fourcc == AVI_avih && chunk.size >= sizeof(avi_main_header_t)) {
            avi_main_header_t header;
            if (!riff_read(demux->reader, &header, sizeof(header))) return;
            demux->info.video.width = header.width;
            demux->info.video.height = header.height;
            demux->info.video.frame_count = header.total_frames;
            demux->info.video.frame_interval_us = header.micro_sec_per_frame;
        } else if (chunk.fourcc == RIFF_ID_LIST) {
            uint32_t type = 0;
            if (!riff_read(demux->reader, &type, sizeof(type))) return;
            if (type == AVI_strl) parse_stream(demux, next);
        }
        mb_seek(demux->reader, next);
    }
}

static bool index_base_valid(avi_demux_t *demux, off_t base, uint32_t first_offset) {
    const off_t position = base + first_offset;
    if (position < 0 || position + (off_t)sizeof(riff_chunk_t) > mb_size(demux->reader)) return false;
    mb_seek(demux->reader, position);
    riff_chunk_t chunk;
    if (!riff_read(demux->reader, &chunk, sizeof(chunk))) return false;
    uint8_t kind = STREAM_NONE;
    return stream_of(demux, chunk.fourcc, &kind) >= 0;
}

static void *alloc_index(size_t bytes) {
    void *data = heap_caps_calloc(1, bytes, MALLOC_CAP_SPIRAM);
    return data ? data : heap_caps_calloc(1, bytes, MALLOC_CAP_DEFAULT);
}

static void build_index(avi_demux_t *demux) {
    if (!demux->idx1_size) return;

    const uint32_t entries = demux->idx1_size / sizeof(avi_index_entry_t);
    if (!entries) return;

    const bool all_key = demux->info.video.codec == AVI_VIDEO_CODEC_MJPEG;
    const uint32_t capacity = entries < AVI_MAX_INDEX_ENTRIES ? entries : AVI_MAX_INDEX_ENTRIES;
    seek_point_t *points = alloc_index(capacity * sizeof(seek_point_t));
    uint8_t *flags = all_key ? NULL : alloc_index((entries + 7) / 8);
    if (!points || (!all_key && !flags)) {
        ESP_LOGW(TAG, "no memory for the frame index; seeking disabled");
        heap_caps_free(points);
        heap_caps_free(flags);
        points = NULL;
        flags = NULL;
    }

    uint32_t video_frames = 0;
    uint32_t keyframes = 0;
    uint32_t stored = 0;
    uint32_t step = 1;
    uint32_t max_video = 0;
    uint32_t max_audio = 0;
    uint32_t first_video_offset = 0;
    bool have_first = false;

    mb_set_readahead(demux->reader, true);
    mb_seek(demux->reader, demux->idx1_offset);
    for (uint32_t i = 0; i < entries; i++) {
        avi_index_entry_t entry;
        if (!riff_read(demux->reader, &entry, sizeof(entry))) break;
        uint8_t kind = STREAM_NONE;
        if (stream_of(demux, entry.chunk_id, &kind) < 0) continue;
        if (kind != STREAM_VIDEO) {
            if (entry.size > max_audio) max_audio = entry.size;
            continue;
        }
        if (!have_first) {
            first_video_offset = entry.offset;
            have_first = true;
        }
        if (entry.size > max_video) max_video = entry.size;
        const uint32_t frame = video_frames++;
        if (!points || (!all_key && !(entry.flags & AVI_INDEX_KEYFRAME))) continue;
        if (flags) flags[frame >> 3] |= (uint8_t)(1u << (frame & 7));
        if (keyframes++ % step != 0) continue;
        if (stored == capacity) {
            for (uint32_t k = 0; k < stored / 2; k++) points[k] = points[k * 2];
            stored /= 2;
            step *= 2;
            if ((keyframes - 1) % step != 0) continue;
        }
        points[stored].frame = frame;
        points[stored].offset = entry.offset;
        stored++;
    }

    if (max_video) demux->info.video.max_frame_bytes = max_video;
    if (max_audio) demux->info.audio.max_frame_bytes = max_audio;
    if (video_frames) demux->info.video.frame_count = video_frames;

    if (!keyframes || !have_first) {
        heap_caps_free(points);
        heap_caps_free(flags);
        return;
    }
    demux->key_flags = flags;
    demux->key_flag_count = flags ? video_frames : 0;

    off_t base = demux->movi_start - 4;
    if (!index_base_valid(demux, base, first_video_offset)) {
        base = 0;
        if (!index_base_valid(demux, base, first_video_offset)) {
            heap_caps_free(points);
            return;
        }
    }

    demux->index_base = base;
    demux->points = points;
    demux->point_count = stored;
    demux->info.seekable = true;
}

avi_demux_t *avi_demux_open(const char *path, const media_arena_t *arena, const char **error) {
    const char *ignored = NULL;
    if (!error) error = &ignored;
    *error = NULL;

    avi_demux_t *demux = heap_caps_calloc(1, sizeof(*demux), MALLOC_CAP_DEFAULT);
    if (!demux) {
        *error = "out of memory";
        return NULL;
    }

    demux->arena = *arena;
    demux->reader = mb_open(path, arena);
    if (!demux->reader) {
        *error = "cannot open the file";
        heap_caps_free(demux);
        return NULL;
    }

    riff_chunk_t riff;
    uint32_t type = 0;
    if (!riff_read(demux->reader, &riff, sizeof(riff)) ||
        !riff_read(demux->reader, &type, sizeof(type)) ||
        riff.fourcc != RIFF_ID_RIFF || type != RIFF_TYPE_AVI) {
        *error = "not an AVI file";
        avi_demux_close(demux);
        return NULL;
    }

    const off_t file_end = mb_size(demux->reader);
    while (mb_tell(demux->reader) + (off_t)sizeof(riff_chunk_t) <= file_end) {
        riff_chunk_t chunk;
        if (!riff_read(demux->reader, &chunk, sizeof(chunk))) break;
        const off_t body = mb_tell(demux->reader);
        off_t next = body + chunk.size + (chunk.size & 1);
        if (next < body || next > file_end) next = file_end;

        if (chunk.fourcc == RIFF_ID_LIST) {
            uint32_t list_type = 0;
            if (!riff_read(demux->reader, &list_type, sizeof(list_type))) break;
            if (list_type == AVI_hdrl) {
                parse_header_list(demux, next);
            } else if (list_type == AVI_movi) {
                demux->movi_start = mb_tell(demux->reader);
                demux->movi_end = next;
            } else if (list_type == RIFF_ID_INFO) {
                riff_read_info(demux->reader, next, &demux->info.tags);
            }
        } else if (chunk.fourcc == AVI_idx1) {
            demux->idx1_offset = body;
            demux->idx1_size = chunk.size;
        } else if (chunk.fourcc == RIFF_ID_id3 || chunk.fourcc == RIFF_ID_ID3) {
            media_tags_read_id3v2(demux->reader, body, NULL, &demux->info.tags);
        }
        mb_seek(demux->reader, next);
    }

    if (!demux->movi_start || demux->movi_end <= demux->movi_start) {
        *error = "no movie data in this AVI";
        avi_demux_close(demux);
        return NULL;
    }
    if (demux->info.video.codec != AVI_VIDEO_CODEC_MJPEG &&
        demux->info.video.codec != AVI_VIDEO_CODEC_H264 &&
        demux->info.video.codec != AVI_VIDEO_CODEC_MPEG2) {
        *error = "not an MJPEG, H.264 or MPEG-2 AVI";
        avi_demux_close(demux);
        return NULL;
    }

    build_index(demux);

    if (demux->info.video.max_frame_bytes > mb_arena_max_view(arena)) {
        *error = "video frames are too large to buffer";
        avi_demux_close(demux);
        return NULL;
    }
    if (!demux->info.video.frame_interval_us) demux->info.video.frame_interval_us = 100000;
    if (!demux->info.video.frame_count) {
        *error = "this AVI declares no frames";
        avi_demux_close(demux);
        return NULL;
    }

    ESP_LOGI(TAG, "%s: %s %ux%u, %u frames, %u us/frame, video <= %u B, audio %s %u Hz x%u",
             path, video_codec_name(demux->info.video.codec),
             (unsigned)demux->info.video.width, (unsigned)demux->info.video.height,
             (unsigned)demux->info.video.frame_count,
             (unsigned)demux->info.video.frame_interval_us,
             (unsigned)demux->info.video.max_frame_bytes,
             riff_audio_codec_name(demux->info.audio.codec),
             (unsigned)demux->info.audio.sample_rate, (unsigned)demux->info.audio.channels);

    mb_seek(demux->reader, demux->movi_start);
    mb_set_readahead(demux->reader, true);
    return demux;
}

void avi_demux_close(avi_demux_t *demux) {
    if (!demux) return;
    media_tags_free(&demux->info.tags);
    if (demux->reader) mb_close(demux->reader);
    heap_caps_free(demux->points);
    heap_caps_free(demux->key_flags);
    heap_caps_free(demux->audio_extra);
    heap_caps_free(demux->video_extra);
    heap_caps_free(demux);
}

const avi_info_t *avi_demux_info(const avi_demux_t *demux) {
    return demux ? &demux->info : NULL;
}

media_buffer_t *avi_demux_buffer(avi_demux_t *demux) {
    return demux ? demux->reader : NULL;
}

static bool starts_idr(const uint8_t *data, uint32_t size, uint8_t length_size) {
    uint32_t pos = 0;
    while (pos < size) {
        uint32_t nal = 0;
        uint32_t nal_size = 0;
        if (length_size) {
            if (size - pos <= length_size) return false;
            for (uint8_t i = 0; i < length_size; i++) nal_size = (nal_size << 8) | data[pos + i];
            nal = pos + length_size;
            pos = nal_size > size - nal ? size : nal + nal_size;
            if (nal_size == 0) continue;
        } else {
            while (pos + 3 < size && !(data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1)) pos++;
            if (pos + 3 >= size) return false;
            nal = pos + 3;
            pos = nal;
        }
        const uint8_t type = data[nal] & 0x1F;
        if (type >= 1 && type <= 5) return type == 5;
    }
    return false;
}

static bool starts_intra_picture(const uint8_t *data, uint32_t size) {
    for (uint32_t pos = 0; pos + 6 <= size; pos++) {
        if (data[pos] == 0 && data[pos + 1] == 0 && data[pos + 2] == 1 && data[pos + 3] == 0) {
            return ((data[pos + 5] >> 3) & 7) == 1;
        }
    }
    return false;
}

static bool keyframe_of(const avi_demux_t *demux, uint32_t frame, const uint8_t *data, uint32_t size) {
    if (demux->info.video.codec == AVI_VIDEO_CODEC_MJPEG) return true;
    if (frame < demux->key_flag_count) return (demux->key_flags[frame >> 3] >> (frame & 7)) & 1;
    if (demux->info.video.codec == AVI_VIDEO_CODEC_MPEG2) return starts_intra_picture(data, size);
    return starts_idr(data, size, demux->nal_length_size);
}

bool avi_demux_read(avi_demux_t *demux, avi_packet_t *packet, bool want_audio) {
    if (!demux || !packet) return false;
    const size_t max_view = mb_arena_max_view(&demux->arena);

    while (mb_tell(demux->reader) + (off_t)sizeof(riff_chunk_t) <= demux->movi_end) {
        const off_t start = mb_tell(demux->reader);
        riff_chunk_t chunk;
        if (!riff_read(demux->reader, &chunk, sizeof(chunk))) return false;
        const off_t next = mb_tell(demux->reader) + chunk.size + (chunk.size & 1);

        uint8_t kind = STREAM_NONE;
        if (stream_of(demux, chunk.fourcc, &kind) < 0) {
            if (chunk.fourcc == RIFF_ID_LIST) {
                uint32_t list_type = 0;
                if (!riff_read(demux->reader, &list_type, sizeof(list_type))) return false;
                continue;
            }
            mb_seek(demux->reader, next);
            continue;
        }

        const bool video = kind == STREAM_VIDEO;
        if ((!video && !want_audio) || chunk.size == 0 || chunk.size > max_view) {
            if (video) demux->next_frame++;
            mb_seek(demux->reader, next);
            continue;
        }

        uint32_t ref = MB_NO_REF;
        const uint8_t *data = mb_view(demux->reader, chunk.size, &ref);
        if (!data) {
            mb_seek(demux->reader, start);
            return false;
        }
        mb_seek(demux->reader, next);

        packet->type = video ? AVI_PACKET_VIDEO : AVI_PACKET_AUDIO;
        packet->data = data;
        packet->size = chunk.size;
        packet->ref = ref;
        packet->frame_index = video ? demux->next_frame++ : 0;
        packet->keyframe = video ? keyframe_of(demux, packet->frame_index, data, chunk.size) : true;
        return true;
    }
    return false;
}

static uint32_t point_at_or_before(const avi_demux_t *demux, uint32_t frame) {
    uint32_t low = 0;
    uint32_t high = demux->point_count;
    while (high - low > 1) {
        const uint32_t middle = low + (high - low) / 2;
        if (demux->points[middle].frame <= frame) {
            low = middle;
        } else {
            high = middle;
        }
    }
    return low;
}

bool avi_demux_seek(avi_demux_t *demux, uint32_t frame, uint32_t *landed_frame) {
    if (!demux) return false;
    uint32_t landed = 0;
    off_t position = demux->movi_start;
    if (frame > 0) {
        if (!demux->points || frame >= demux->info.video.frame_count) return false;
        const uint32_t low = point_at_or_before(demux, frame);
        if (demux->points[low].frame <= frame) {
            landed = demux->points[low].frame;
            position = demux->index_base + demux->points[low].offset;
        }
    }
    mb_seek(demux->reader, position);
    demux->next_frame = landed;
    if (landed_frame) *landed_frame = landed;
    return true;
}

bool avi_demux_keyframe_before(const avi_demux_t *demux, uint32_t frame, uint32_t *key_frame) {
    if (!demux || !demux->points || demux->info.video.codec == AVI_VIDEO_CODEC_MJPEG) return false;
    const uint32_t key = demux->points[point_at_or_before(demux, frame)].frame;
    if (key > frame) return false;
    *key_frame = key;
    return true;
}
