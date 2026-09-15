/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media_types.hpp"
#include <memory>
#include <string>

class Demuxer {
public:
    virtual ~Demuxer() = default;

    virtual bool open(const std::string &path) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool read(uint8_t *video, std::size_t video_capacity,
                      uint8_t *audio, std::size_t audio_capacity, Packet *out) = 0;
    virtual bool seek(int64_t pts_us) = 0;

    const MediaInfo &info() const { return info_; }
    const std::string &error() const { return error_; }

protected:
    MediaInfo info_;
    std::string error_;
};

bool demuxer_supports(const char *name);
std::unique_ptr<Demuxer> demuxer_create(const std::string &path);
