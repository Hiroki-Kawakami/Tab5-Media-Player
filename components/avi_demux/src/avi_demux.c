/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "avi_demux.h"

#include <stdlib.h>
#include <string.h>

#include "avi_format.h"
#include "buffered_reader.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "avi_demux";

#define AVI_MAX_STREAMS 8
#define AVI_MAX_INDEX_ENTRIES 60000
#define AVI_DEFAULT_VIDEO_BYTES (512 * 1024)
#define AVI_DEFAULT_AUDIO_BYTES (32 * 1024)

enum {
    STREAM_NONE = 0,
    STREAM_VIDEO,
    STREAM_AUDIO,
};

struct avi_demux {
    buffered_reader_t *reader;
    avi_info_t info;
    off_t movi_start;
    off_t movi_end;
    off_t idx1_offset;
    uint32_t idx1_size;
    off_t index_base;
    uint32_t *offsets;
    uint32_t index_count;
    uint32_t index_step;
    uint32_t next_frame;
    uint8_t stream_kind[AVI_MAX_STREAMS];
    uint8_t stream_count;
};

static bool read_exact(buffered_reader_t *reader, void *buffer, size_t size) {
    return br_read(reader, buffer, size) == size;
}

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
    default:
        return AVI_VIDEO_CODEC_UNSUPPORTED;
    }
}

static avi_audio_codec_t audio_codec_of(uint16_t format_tag) {
    switch (format_tag) {
    case AVI_WAVE_FORMAT_PCM:
        return AVI_AUDIO_CODEC_PCM;
    case AVI_WAVE_FORMAT_MP3:
        return AVI_AUDIO_CODEC_MP3;
    default:
        return AVI_AUDIO_CODEC_UNSUPPORTED;
    }
}

static void parse_stream(avi_demux_t *demux, off_t list_end) {
    avi_stream_header_t strh;
    memset(&strh, 0, sizeof(strh));
    bool have_header = false;
    const int index = demux->stream_count < AVI_MAX_STREAMS ? demux->stream_count : -1;

    while (br_tell(demux->reader) + (off_t)sizeof(avi_chunk_t) <= list_end) {
        avi_chunk_t chunk;
        if (!read_exact(demux->reader, &chunk, sizeof(chunk))) return;
        const off_t body = br_tell(demux->reader);
        const off_t next = body + chunk.size + (chunk.size & 1);

        if (chunk.fourcc == AVI_strh && chunk.size >= sizeof(strh)) {
            if (!read_exact(demux->reader, &strh, sizeof(strh))) return;
            have_header = true;
        } else if (chunk.fourcc == AVI_strf && have_header && index >= 0) {
            if (strh.fourcc_type == AVI_vids && chunk.size >= sizeof(avi_bitmap_info_t)) {
                avi_bitmap_info_t bitmap;
                if (!read_exact(demux->reader, &bitmap, sizeof(bitmap))) return;
                demux->stream_kind[index] = STREAM_VIDEO;
                demux->info.video.codec = video_codec_of(bitmap.compression);
                if (bitmap.width) demux->info.video.width = bitmap.width;
                if (bitmap.height) demux->info.video.height = bitmap.height;
                if (strh.rate && strh.scale) {
                    demux->info.video.frame_interval_us =
                        (uint32_t)((uint64_t)strh.scale * 1000000ull / strh.rate);
                }
                if (strh.length) demux->info.video.frame_count = strh.length;
            } else if (strh.fourcc_type == AVI_auds && chunk.size >= sizeof(avi_wave_format_t)) {
                avi_wave_format_t wave;
                if (!read_exact(demux->reader, &wave, sizeof(wave))) return;
                demux->stream_kind[index] = STREAM_AUDIO;
                demux->info.audio.codec = audio_codec_of(wave.format_tag);
                demux->info.audio.channels = (uint8_t)wave.channels;
                demux->info.audio.sample_rate = wave.samples_per_sec;
                demux->info.audio.bits_per_sample =
                    wave.bits_per_sample ? (uint8_t)wave.bits_per_sample : 16;
            }
        }
        br_seek(demux->reader, next);
    }

    if (index >= 0) demux->stream_count++;
}

static void parse_header_list(avi_demux_t *demux, off_t list_end) {
    while (br_tell(demux->reader) + (off_t)sizeof(avi_chunk_t) <= list_end) {
        avi_chunk_t chunk;
        if (!read_exact(demux->reader, &chunk, sizeof(chunk))) return;
        const off_t body = br_tell(demux->reader);
        const off_t next = body + chunk.size + (chunk.size & 1);

        if (chunk.fourcc == AVI_avih && chunk.size >= sizeof(avi_main_header_t)) {
            avi_main_header_t header;
            if (!read_exact(demux->reader, &header, sizeof(header))) return;
            demux->info.video.width = header.width;
            demux->info.video.height = header.height;
            demux->info.video.frame_count = header.total_frames;
            demux->info.video.frame_interval_us = header.micro_sec_per_frame;
        } else if (chunk.fourcc == AVI_LIST) {
            uint32_t type = 0;
            if (!read_exact(demux->reader, &type, sizeof(type))) return;
            if (type == AVI_strl) parse_stream(demux, next);
        }
        br_seek(demux->reader, next);
    }
}

static bool index_base_valid(avi_demux_t *demux, off_t base, uint32_t first_offset) {
    const off_t position = base + first_offset;
    if (position < 0 || position + (off_t)sizeof(avi_chunk_t) > br_size(demux->reader)) return false;
    br_seek(demux->reader, position);
    avi_chunk_t chunk;
    if (!read_exact(demux->reader, &chunk, sizeof(chunk))) return false;
    uint8_t kind = STREAM_NONE;
    return stream_of(demux, chunk.fourcc, &kind) >= 0;
}

static void build_index(avi_demux_t *demux) {
    if (!demux->idx1_size) return;

    const uint32_t entries = demux->idx1_size / sizeof(avi_index_entry_t);
    if (!entries) return;

    uint32_t step = 1;
    uint32_t capacity = entries;
    while (capacity > AVI_MAX_INDEX_ENTRIES) {
        step++;
        capacity = (entries + step - 1) / step;
    }

    uint32_t *offsets = heap_caps_malloc(capacity * sizeof(uint32_t), MALLOC_CAP_SPIRAM);
    if (!offsets) offsets = heap_caps_malloc(capacity * sizeof(uint32_t), MALLOC_CAP_DEFAULT);
    if (!offsets) ESP_LOGW(TAG, "no memory for the frame index; seeking disabled");

    uint32_t video_frames = 0;
    uint32_t stored = 0;
    uint32_t max_video = 0;
    uint32_t max_audio = 0;
    uint32_t first_video_offset = 0;
    bool have_first = false;

    br_set_readahead(demux->reader, true);
    br_seek(demux->reader, demux->idx1_offset);
    for (uint32_t i = 0; i < entries; i++) {
        avi_index_entry_t entry;
        if (!read_exact(demux->reader, &entry, sizeof(entry))) break;
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
        if (offsets && video_frames % step == 0 && stored < capacity) {
            offsets[stored++] = entry.offset;
        }
        video_frames++;
    }

    if (max_video) demux->info.video.max_frame_bytes = max_video;
    if (max_audio) demux->info.audio.max_frame_bytes = max_audio;
    if (video_frames) demux->info.video.frame_count = video_frames;

    if (!offsets) return;
    if (!have_first || !stored) {
        heap_caps_free(offsets);
        return;
    }

    off_t base = demux->movi_start - 4;
    if (!index_base_valid(demux, base, first_video_offset)) {
        base = 0;
        if (!index_base_valid(demux, base, first_video_offset)) {
            heap_caps_free(offsets);
            return;
        }
    }

    demux->index_base = base;
    demux->offsets = offsets;
    demux->index_count = stored;
    demux->index_step = step;
    demux->info.seekable = true;
}

avi_demux_t *avi_demux_open(const char *path, const char **error) {
    const char *ignored = NULL;
    if (!error) error = &ignored;
    *error = NULL;

    avi_demux_t *demux = heap_caps_calloc(1, sizeof(*demux), MALLOC_CAP_DEFAULT);
    if (!demux) {
        *error = "out of memory";
        return NULL;
    }

    demux->reader = br_open(path);
    if (!demux->reader) {
        *error = "cannot open the file";
        heap_caps_free(demux);
        return NULL;
    }

    avi_chunk_t riff;
    uint32_t type = 0;
    if (!read_exact(demux->reader, &riff, sizeof(riff)) ||
        !read_exact(demux->reader, &type, sizeof(type)) ||
        riff.fourcc != AVI_RIFF || type != AVI_TYPE) {
        *error = "not an AVI file";
        avi_demux_close(demux);
        return NULL;
    }

    const off_t file_end = br_size(demux->reader);
    while (br_tell(demux->reader) + (off_t)sizeof(avi_chunk_t) <= file_end) {
        avi_chunk_t chunk;
        if (!read_exact(demux->reader, &chunk, sizeof(chunk))) break;
        const off_t body = br_tell(demux->reader);
        off_t next = body + chunk.size + (chunk.size & 1);
        if (next < body || next > file_end) next = file_end;

        if (chunk.fourcc == AVI_LIST) {
            uint32_t list_type = 0;
            if (!read_exact(demux->reader, &list_type, sizeof(list_type))) break;
            if (list_type == AVI_hdrl) {
                parse_header_list(demux, next);
            } else if (list_type == AVI_movi) {
                demux->movi_start = br_tell(demux->reader);
                demux->movi_end = next;
            }
        } else if (chunk.fourcc == AVI_idx1) {
            demux->idx1_offset = body;
            demux->idx1_size = chunk.size;
        }
        br_seek(demux->reader, next);
    }

    if (!demux->movi_start || demux->movi_end <= demux->movi_start) {
        *error = "no movie data in this AVI";
        avi_demux_close(demux);
        return NULL;
    }
    if (demux->info.video.codec != AVI_VIDEO_CODEC_MJPEG) {
        *error = "not an MJPEG AVI";
        avi_demux_close(demux);
        return NULL;
    }

    build_index(demux);

    if (!demux->info.video.max_frame_bytes) {
        demux->info.video.max_frame_bytes = AVI_DEFAULT_VIDEO_BYTES;
    }
    if (demux->info.audio.codec != AVI_AUDIO_CODEC_NONE && !demux->info.audio.max_frame_bytes) {
        demux->info.audio.max_frame_bytes = AVI_DEFAULT_AUDIO_BYTES;
    }
    if (!demux->info.video.frame_interval_us) demux->info.video.frame_interval_us = 100000;
    if (!demux->info.video.frame_count) {
        *error = "this AVI declares no frames";
        avi_demux_close(demux);
        return NULL;
    }

    ESP_LOGI(TAG, "%s: %ux%u, %u frames, %u us/frame, video <= %u B, audio %s %u Hz x%u",
             path, (unsigned)demux->info.video.width, (unsigned)demux->info.video.height,
             (unsigned)demux->info.video.frame_count,
             (unsigned)demux->info.video.frame_interval_us,
             (unsigned)demux->info.video.max_frame_bytes,
             demux->info.audio.codec == AVI_AUDIO_CODEC_PCM ? "PCM"
                 : demux->info.audio.codec == AVI_AUDIO_CODEC_MP3 ? "MP3"
                 : demux->info.audio.codec == AVI_AUDIO_CODEC_NONE ? "none" : "unsupported",
             (unsigned)demux->info.audio.sample_rate, (unsigned)demux->info.audio.channels);

    br_seek(demux->reader, demux->movi_start);
    br_set_readahead(demux->reader, true);
    return demux;
}

void avi_demux_close(avi_demux_t *demux) {
    if (!demux) return;
    if (demux->reader) br_close(demux->reader);
    heap_caps_free(demux->offsets);
    heap_caps_free(demux);
}

const avi_info_t *avi_demux_info(const avi_demux_t *demux) {
    return demux ? &demux->info : NULL;
}

bool avi_demux_read(avi_demux_t *demux, avi_packet_t *packet,
                    uint8_t *video, uint32_t video_capacity,
                    uint8_t *audio, uint32_t audio_capacity) {
    if (!demux || !packet) return false;

    while (br_tell(demux->reader) + (off_t)sizeof(avi_chunk_t) <= demux->movi_end) {
        avi_chunk_t chunk;
        if (!read_exact(demux->reader, &chunk, sizeof(chunk))) return false;
        const off_t next = br_tell(demux->reader) + chunk.size + (chunk.size & 1);

        uint8_t kind = STREAM_NONE;
        if (stream_of(demux, chunk.fourcc, &kind) < 0) {
            if (chunk.fourcc == AVI_LIST) {
                uint32_t list_type = 0;
                if (!read_exact(demux->reader, &list_type, sizeof(list_type))) return false;
                (void)list_type;
                continue;
            }
            br_seek(demux->reader, next);
            continue;
        }

        uint8_t *destination = kind == STREAM_VIDEO ? video : audio;
        const uint32_t capacity = kind == STREAM_VIDEO ? video_capacity : audio_capacity;
        if (!destination || chunk.size > capacity) {
            if (kind == STREAM_VIDEO) demux->next_frame++;
            br_seek(demux->reader, next);
            continue;
        }

        if (chunk.size && !read_exact(demux->reader, destination, chunk.size)) return false;
        br_seek(demux->reader, next);

        packet->type = kind == STREAM_VIDEO ? AVI_PACKET_VIDEO : AVI_PACKET_AUDIO;
        packet->size = chunk.size;
        packet->frame_index = kind == STREAM_VIDEO ? demux->next_frame++ : 0;
        return true;
    }
    return false;
}

bool avi_demux_seek(avi_demux_t *demux, uint32_t frame) {
    if (!demux) return false;
    if (frame == 0) {
        br_seek(demux->reader, demux->movi_start);
        demux->next_frame = 0;
        return true;
    }
    if (!demux->offsets || !demux->index_count) return false;

    const uint32_t entry = frame / demux->index_step;
    if (entry >= demux->index_count) return false;

    br_seek(demux->reader, demux->index_base + demux->offsets[entry]);
    demux->next_frame = entry * demux->index_step;
    return true;
}
