/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "avi_demux.h"

#include <stdlib.h>
#include <string.h>

#include "avi_format.h"
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

struct avi_demux {
    media_buffer_t *reader;
    media_arena_t arena;
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
    uint8_t *audio_extra;
};

static bool read_exact(media_buffer_t *reader, void *buffer, size_t size) {
    return mb_read(reader, buffer, size) == size;
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
    case AVI_WAVE_FORMAT_IMA_ADPCM:
        return AVI_AUDIO_CODEC_ADPCM_IMA;
    case AVI_WAVE_FORMAT_AAC:
    case AVI_WAVE_FORMAT_AAC_ADTS:
    case AVI_WAVE_FORMAT_AAC_FAAD:
        return AVI_AUDIO_CODEC_AAC;
    default:
        return AVI_AUDIO_CODEC_UNSUPPORTED;
    }
}

static const char *audio_codec_name(avi_audio_codec_t codec) {
    switch (codec) {
    case AVI_AUDIO_CODEC_NONE: return "none";
    case AVI_AUDIO_CODEC_PCM: return "PCM";
    case AVI_AUDIO_CODEC_MP3: return "MP3";
    case AVI_AUDIO_CODEC_ADPCM_IMA: return "IMA ADPCM";
    case AVI_AUDIO_CODEC_AAC: return "AAC";
    default: return "unsupported";
    }
}

static void read_wave_extra(avi_demux_t *demux, uint32_t available) {
    heap_caps_free(demux->audio_extra);
    demux->audio_extra = NULL;
    demux->info.audio.codec_private = NULL;
    demux->info.audio.codec_private_size = 0;

    uint16_t extra_size = 0;
    if (available < sizeof(extra_size) || !read_exact(demux->reader, &extra_size, sizeof(extra_size))) {
        return;
    }
    available -= sizeof(extra_size);
    if (extra_size > available) extra_size = (uint16_t)available;
    if (extra_size == 0 || extra_size > AVI_WAVE_FORMAT_EXTRA_LIMIT) return;

    uint8_t *extra = heap_caps_malloc(extra_size, MALLOC_CAP_DEFAULT);
    if (!extra) return;
    if (!read_exact(demux->reader, extra, extra_size)) {
        heap_caps_free(extra);
        return;
    }
    demux->audio_extra = extra;
    demux->info.audio.codec_private = extra;
    demux->info.audio.codec_private_size = extra_size;
}

static void parse_stream(avi_demux_t *demux, off_t list_end) {
    avi_stream_header_t strh;
    memset(&strh, 0, sizeof(strh));
    bool have_header = false;
    const int index = demux->stream_count < AVI_MAX_STREAMS ? demux->stream_count : -1;

    while (mb_tell(demux->reader) + (off_t)sizeof(avi_chunk_t) <= list_end) {
        avi_chunk_t chunk;
        if (!read_exact(demux->reader, &chunk, sizeof(chunk))) return;
        const off_t body = mb_tell(demux->reader);
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
                demux->info.audio.block_align = wave.block_align;
                read_wave_extra(demux, chunk.size - sizeof(wave));
            }
        }
        mb_seek(demux->reader, next);
    }

    if (index >= 0) demux->stream_count++;
}

static void parse_header_list(avi_demux_t *demux, off_t list_end) {
    while (mb_tell(demux->reader) + (off_t)sizeof(avi_chunk_t) <= list_end) {
        avi_chunk_t chunk;
        if (!read_exact(demux->reader, &chunk, sizeof(chunk))) return;
        const off_t body = mb_tell(demux->reader);
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
        mb_seek(demux->reader, next);
    }
}

static bool index_base_valid(avi_demux_t *demux, off_t base, uint32_t first_offset) {
    const off_t position = base + first_offset;
    if (position < 0 || position + (off_t)sizeof(avi_chunk_t) > mb_size(demux->reader)) return false;
    mb_seek(demux->reader, position);
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

    mb_set_readahead(demux->reader, true);
    mb_seek(demux->reader, demux->idx1_offset);
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

    avi_chunk_t riff;
    uint32_t type = 0;
    if (!read_exact(demux->reader, &riff, sizeof(riff)) ||
        !read_exact(demux->reader, &type, sizeof(type)) ||
        riff.fourcc != AVI_RIFF || type != AVI_TYPE) {
        *error = "not an AVI file";
        avi_demux_close(demux);
        return NULL;
    }

    const off_t file_end = mb_size(demux->reader);
    while (mb_tell(demux->reader) + (off_t)sizeof(avi_chunk_t) <= file_end) {
        avi_chunk_t chunk;
        if (!read_exact(demux->reader, &chunk, sizeof(chunk))) break;
        const off_t body = mb_tell(demux->reader);
        off_t next = body + chunk.size + (chunk.size & 1);
        if (next < body || next > file_end) next = file_end;

        if (chunk.fourcc == AVI_LIST) {
            uint32_t list_type = 0;
            if (!read_exact(demux->reader, &list_type, sizeof(list_type))) break;
            if (list_type == AVI_hdrl) {
                parse_header_list(demux, next);
            } else if (list_type == AVI_movi) {
                demux->movi_start = mb_tell(demux->reader);
                demux->movi_end = next;
            }
        } else if (chunk.fourcc == AVI_idx1) {
            demux->idx1_offset = body;
            demux->idx1_size = chunk.size;
        }
        mb_seek(demux->reader, next);
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

    ESP_LOGI(TAG, "%s: %ux%u, %u frames, %u us/frame, video <= %u B, audio %s %u Hz x%u",
             path, (unsigned)demux->info.video.width, (unsigned)demux->info.video.height,
             (unsigned)demux->info.video.frame_count,
             (unsigned)demux->info.video.frame_interval_us,
             (unsigned)demux->info.video.max_frame_bytes,
             audio_codec_name(demux->info.audio.codec),
             (unsigned)demux->info.audio.sample_rate, (unsigned)demux->info.audio.channels);

    mb_seek(demux->reader, demux->movi_start);
    mb_set_readahead(demux->reader, true);
    return demux;
}

void avi_demux_close(avi_demux_t *demux) {
    if (!demux) return;
    if (demux->reader) mb_close(demux->reader);
    heap_caps_free(demux->offsets);
    heap_caps_free(demux->audio_extra);
    heap_caps_free(demux);
}

const avi_info_t *avi_demux_info(const avi_demux_t *demux) {
    return demux ? &demux->info : NULL;
}

media_buffer_t *avi_demux_buffer(avi_demux_t *demux) {
    return demux ? demux->reader : NULL;
}

bool avi_demux_read(avi_demux_t *demux, avi_packet_t *packet, bool want_audio) {
    if (!demux || !packet) return false;
    const size_t max_view = mb_arena_max_view(&demux->arena);

    while (mb_tell(demux->reader) + (off_t)sizeof(avi_chunk_t) <= demux->movi_end) {
        const off_t start = mb_tell(demux->reader);
        avi_chunk_t chunk;
        if (!read_exact(demux->reader, &chunk, sizeof(chunk))) return false;
        const off_t next = mb_tell(demux->reader) + chunk.size + (chunk.size & 1);

        uint8_t kind = STREAM_NONE;
        if (stream_of(demux, chunk.fourcc, &kind) < 0) {
            if (chunk.fourcc == AVI_LIST) {
                uint32_t list_type = 0;
                if (!read_exact(demux->reader, &list_type, sizeof(list_type))) return false;
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
        return true;
    }
    return false;
}

bool avi_demux_seek(avi_demux_t *demux, uint32_t frame) {
    if (!demux) return false;
    if (frame == 0) {
        mb_seek(demux->reader, demux->movi_start);
        demux->next_frame = 0;
        return true;
    }
    if (!demux->offsets || !demux->index_count) return false;

    const uint32_t entry = frame / demux->index_step;
    if (entry >= demux->index_count) return false;

    mb_seek(demux->reader, demux->index_base + demux->offsets[entry]);
    demux->next_frame = entry * demux->index_step;
    return true;
}
