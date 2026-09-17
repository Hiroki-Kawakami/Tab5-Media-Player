/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media_types.hpp"
#include "media_buffer.h"
#include <memory>
#include <string>

class Demuxer {
public:
    virtual ~Demuxer() = default;

    virtual bool open(const std::string &path, const media_arena_t &arena) = 0;
    virtual void close() = 0;
    virtual bool isOpen() const = 0;
    virtual bool read(bool want_audio, Packet *out) = 0;
    virtual bool seek(int64_t pts_us) = 0;

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

bool demuxer_supports(const char *name);
std::unique_ptr<Demuxer> demuxer_create(const std::string &path);
