/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media_types.hpp"
#include "media_buffer.h"
#include "media_tags.h"
#include <memory>
#include <string>

class Demuxer {
public:
    virtual ~Demuxer() = default;

    virtual bool open(const std::string &path, const media_arena_t &arena,
                      bool want_cover) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool read(bool want_audio, Packet *out) = 0;
    virtual bool seek(int64_t pts_us, int64_t *landed_us) = 0;
    virtual bool keyframeBefore(int64_t, int64_t *) const { return false; }

    off_t bytes() const { return buffer_ ? mb_size(buffer_) : 0; }
    void release(uint32_t ref) { mb_release(buffer_, ref); }
    void releaseAll() { mb_release_all(buffer_); }
    void interrupt(bool interrupted) { mb_interrupt(buffer_, interrupted); }

    const MediaInfo &info() const { return info_; }
    const std::string &error() const { return error_; }

protected:
    media_buffer_t *buffer_ = nullptr;
    MediaInfo info_;
    std::string error_;
};

/* Moves the picture out of `tags` instead of copying it, so the bytes stay in
   the buffer the tag reader read them into. */
void demuxer_apply_tags(const media_tags_t &tags, MediaInfo *info);

MediaSummary media_summary_make(const std::string &path, const MediaInfo &info, int64_t file_bytes,
                                const std::string &audio_note);

bool h264_config_to_annexb(const uint8_t *data, std::size_t size, std::vector<uint8_t> *annexb,
                           uint8_t *nal_length_size);

enum class MediaKind {
    None,
    Video,
    Audio,
};

MediaKind demuxer_media_kind(const char *name);
const char *demuxer_format_name(const std::string &path);
std::unique_ptr<Demuxer> demuxer_create(const std::string &path);
