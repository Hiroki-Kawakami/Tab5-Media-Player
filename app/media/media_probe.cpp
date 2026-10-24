/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "media_probe.hpp"
#include "demuxer.hpp"
#include "media/psram_allocator.hpp"
#include "esp_log.h"

#include <algorithm>

static const char *TAG = "media_probe";

static constexpr int64_t kKeyframeAtUs = 10 * 1000 * 1000;
static constexpr uint32_t kMaxKeyframeBytes = 2 * 1024 * 1024;
static constexpr uint32_t kMjpegHeadBytes = 16 * 1024;

bool media_probe(const std::string &path, const medf_arena_t &arena, bool want_cover,
                 MediaSummary *out, std::string *error) {
    *out = {};
    error->clear();

    std::unique_ptr<Demuxer> demuxer = demuxer_create(path);
    if (!demuxer) {
        *error = "unsupported file";
        return false;
    }
    if (!demuxer->open(path, arena, want_cover)) {
        *error = demuxer->error();
        if (error->empty()) *error = "cannot open";
        demuxer->close();
        return false;
    }

    *out = media_summary_make(path, demuxer->info(), demuxer->bytes(), {});
    demuxer->close();
    return true;
}

CoverArt media_probe_cover(const std::string &path, const medf_arena_t &arena,
                           const CoverLocation &at) {
    CoverArt cover;
    if (!at) return cover;

    medf_buffer_t *reader = medf_buffer_open(path.c_str(), &arena);
    if (!reader) return cover;

    void *owner = nullptr;
    uint8_t *data = medf_buffer_read_alloc(reader, (off_t)at.offset, at.size, &owner);
    medf_buffer_close(reader);
    if (!data) return cover;

    cover.data = psram_make_shared<CoverBytes>((uint8_t *)owner, data, at.size);
    cover.format = at.size >= 4 && data[0] == 0x89 && data[1] == 'P' ? CoverFormat::Png
                                                                     : CoverFormat::Jpeg;
    return cover;
}

bool media_probe_keyframe(const std::string &path, const medf_arena_t &arena,
                          VideoKeyframe *out) {
    *out = {};
    std::unique_ptr<Demuxer> demuxer = demuxer_create(path);
    if (!demuxer || !demuxer->open(path, arena, false)) return false;
    const MediaInfo &info = demuxer->info();
    const CodecId codec = info.video.codec;
    if (codec != CodecId::Mjpeg && codec != CodecId::H264 && codec != CodecId::Mpeg2) {
        demuxer->close();
        return false;
    }

    const int64_t at_us =
        info.duration_us >= 2 * kKeyframeAtUs ? kKeyframeAtUs : info.duration_us / 2;
    int64_t offset = 0;
    uint32_t size = 0;
    if (!demuxer->keyframeAt(at_us, &offset, &size) &&
        (at_us == 0 || !demuxer->keyframeAt(0, &offset, &size))) {
        ESP_LOGW(TAG, "no keyframe in %s", path.c_str());
        demuxer->close();
        return false;
    }
    if (size > kMaxKeyframeBytes) {
        ESP_LOGW(TAG, "%u byte keyframe in %s", (unsigned)size, path.c_str());
        demuxer->close();
        return false;
    }

    const uint32_t read = codec == CodecId::Mjpeg ? std::min(size, kMjpegHeadBytes) : size;
    void *owner = nullptr;
    uint8_t *data = demuxer->readAlloc(offset, read, &owner);
    if (!data) {
        ESP_LOGE(TAG, "cannot read %u bytes at %lld in %s", (unsigned)read, (long long)offset,
                 path.c_str());
        demuxer->close();
        return false;
    }
    out->codec = codec;
    out->rotation = info.video.rotation;
    out->at = { offset, size };
    out->bytes = psram_make_shared<CoverBytes>((uint8_t *)owner, data, read);
    out->config.assign(info.video.codec_private.begin(), info.video.codec_private.end());
    out->nal_length_size = info.video.nal_length_size;
    demuxer->close();
    return true;
}
