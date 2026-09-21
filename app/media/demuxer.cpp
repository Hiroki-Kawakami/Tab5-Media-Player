/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "demuxer.hpp"
#include "h264_dec.h"
#include "esp_heap_caps.h"
#include <cstring>
#include <strings.h>

std::unique_ptr<Demuxer> avi_demuxer_create();
std::unique_ptr<Demuxer> mkv_demuxer_create();
std::unique_ptr<Demuxer> mp4_demuxer_create();
std::unique_ptr<Demuxer> wav_demuxer_create();
std::unique_ptr<Demuxer> es_audio_demuxer_create();

namespace {

struct Format {
    const char *suffix;
    const char *name;
    MediaKind kind;
    std::unique_ptr<Demuxer> (*create)();
};

constexpr Format kFormats[] = {
    { ".avi", "AVI", MediaKind::Video, avi_demuxer_create },
    { ".mkv", "Matroska", MediaKind::Video, mkv_demuxer_create },
    { ".mp4", "MP4", MediaKind::Video, mp4_demuxer_create },
    { ".m4v", "MP4", MediaKind::Video, mp4_demuxer_create },
    { ".mov", "QuickTime", MediaKind::Video, mp4_demuxer_create },
    { ".m4a", "MP4", MediaKind::Audio, mp4_demuxer_create },
    { ".wav", "WAVE", MediaKind::Audio, wav_demuxer_create },
    { ".mp3", "MP3", MediaKind::Audio, es_audio_demuxer_create },
    { ".aac", "AAC", MediaKind::Audio, es_audio_demuxer_create },
};

bool starts_with_start_code(const uint8_t *data, std::size_t size) {
    return (size >= 3 && data[0] == 0 && data[1] == 0 && data[2] == 1) ||
           (size >= 4 && data[0] == 0 && data[1] == 0 && data[2] == 0 && data[3] == 1);
}

bool append_nal_units(const uint8_t *data, std::size_t size, std::size_t *pos, std::size_t count,
                      std::vector<uint8_t> *out) {
    static constexpr uint8_t kStartCode[] = { 0, 0, 0, 1 };
    for (std::size_t i = 0; i < count; i++) {
        if (size - *pos < 2) return false;
        const std::size_t length = (std::size_t)(data[*pos] << 8 | data[*pos + 1]);
        *pos += 2;
        if (size - *pos < length) return false;
        out->insert(out->end(), kStartCode, kStartCode + sizeof(kStartCode));
        out->insert(out->end(), data + *pos, data + *pos + length);
        *pos += length;
    }
    return true;
}

const Format *format_of(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return nullptr;
    for (const Format &format : kFormats) {
        if (strcasecmp(dot, format.suffix) == 0) return &format;
    }
    return nullptr;
}

}

bool h264_config_to_annexb(const uint8_t *data, std::size_t size, std::vector<uint8_t> *annexb,
                           uint8_t *nal_length_size) {
    annexb->clear();
    *nal_length_size = 0;
    if (!data || size == 0) return false;
    if (starts_with_start_code(data, size)) {
        annexb->assign(data, data + size);
        return true;
    }
    if (size < 7 || data[0] != 1 || (data[4] & 0x03) == 2) return false;

    std::size_t pos = 6;
    if (!append_nal_units(data, size, &pos, data[5] & 0x1F, annexb) || pos >= size) {
        annexb->clear();
        return false;
    }
    const std::size_t pps_count = data[pos++];
    if (!append_nal_units(data, size, &pos, pps_count, annexb)) {
        annexb->clear();
        return false;
    }
    *nal_length_size = (uint8_t)((data[4] & 0x03) + 1);
    return true;
}

CoverBytes::CoverBytes(uint8_t *owner, const uint8_t *bytes, std::size_t count)
    : owner(owner), bytes(bytes), count(count) {}

CoverBytes::~CoverBytes() {
    heap_caps_free(owner);
}

void demuxer_apply_tags(const media_tags_t &tags, MediaInfo *info) {
    info->tags.title = media_tags_get(&tags, MEDIA_TAG_TITLE);
    info->tags.artist = media_tags_get(&tags, MEDIA_TAG_ARTIST);
    info->tags.album = media_tags_get(&tags, MEDIA_TAG_ALBUM);
    info->tags.album_artist = media_tags_get(&tags, MEDIA_TAG_ALBUM_ARTIST);
    info->tags.track = media_tags_get(&tags, MEDIA_TAG_TRACK);
    info->tags.date = media_tags_get(&tags, MEDIA_TAG_DATE);

    info->cover_at = { (int64_t)tags.cover_at.offset, tags.cover_at.size };
    info->cover_scanned = tags.cover_scanned;
    if (!tags.cover.data || !tags.cover.size) return;
    /* The tag struct lives in the demuxer's own allocation, never in read-only
       memory: taking the picture out of it is what keeps the bytes uncopied. */
    const media_cover_t cover = media_tags_take_cover(const_cast<media_tags_t *>(&tags));
    info->cover.data = psram_make_shared<CoverBytes>(cover.owner, cover.data, cover.size);
    info->cover.format = cover.format == MEDIA_COVER_PNG ? CoverFormat::Png : CoverFormat::Jpeg;
}

MediaSummary media_summary_make(const std::string &path, const MediaInfo &info, int64_t file_bytes,
                                const std::string &audio_note) {
    MediaSummary summary;
    summary.valid = true;
    summary.container = demuxer_format_name(path);
    summary.file_bytes = file_bytes;
    summary.duration_us = info.duration_us;
    summary.seekable = info.seekable;
    summary.tags = info.tags;
    summary.cover = info.cover;
    summary.cover_at = info.cover_at;
    summary.cover_scanned = info.cover_scanned;
    summary.video.codec = info.video.codec;
    summary.video.width = info.video.width;
    summary.video.height = info.video.height;
    summary.video.frame_interval_us = info.frame_interval_us;
    summary.video.rotation = info.video.rotation;
    if (info.video.codec == CodecId::H264 && !info.video.codec_private.empty()) {
        h264_dec_stream_info_t stream = {};
        const char *failure = nullptr;
        if (h264_dec_probe(info.video.codec_private.data(), info.video.codec_private.size(), 0,
                           &stream, &failure)) {
            summary.video.profile_idc = stream.profile_idc;
            summary.video.level_idc = stream.level_idc;
        }
    }
    summary.audio.codec = info.audio.codec;
    summary.audio.sample_rate = info.audio.sample_rate;
    summary.audio.bitrate_bps = info.audio.bitrate_bps;
    summary.audio.channels = info.audio.channels;
    summary.audio.bits = info.audio.bits;
    if (!summary.audio.bitrate_bps && info.audio.codec == CodecId::Pcm) {
        summary.audio.bitrate_bps = info.audio.sample_rate * info.audio.channels * info.audio.bits;
    }
    summary.audio.note = audio_note;
    return summary;
}

MediaKind demuxer_media_kind(const char *name) {
    const Format *format = format_of(name);
    return format ? format->kind : MediaKind::None;
}

const char *demuxer_format_name(const std::string &path) {
    const Format *format = format_of(path.c_str());
    return format ? format->name : "";
}

std::unique_ptr<Demuxer> demuxer_create(const std::string &path) {
    const Format *format = format_of(path.c_str());
    return format ? format->create() : nullptr;
}
