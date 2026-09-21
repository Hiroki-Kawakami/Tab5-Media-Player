/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/artwork_codec.hpp"
#include "media/media_types.hpp"
#include "media_buffer.h"
#include "media_tags.h"
#include <cstdint>
#include <memory>
#include <string>

struct MediaEntry {
    bool ok = false;
    bool has_cover = false;
    uint64_t image_id = 0;
    int64_t duration_us = 0;
    CodecId audio_codec = CodecId::None;
    uint32_t sample_rate = 0;
    uint32_t bitrate_bps = 0;
    uint8_t channels = 0;
    uint8_t bits = 0;
    char title[MEDIA_TAG_TEXT_BYTES] = {};
    char artist[MEDIA_TAG_TEXT_BYTES] = {};
    char album[MEDIA_TAG_TEXT_BYTES] = {};
};

enum MetaWant : uint8_t {
    MetaWantInfo = 1,
    MetaWantImage = 2,
};

enum class MetaPriority : uint8_t {
    Blocking,
    Visible,
    Idle,
};

void media_cache_init(const media_arena_t &arena);
void media_cache_start();
void media_cache_stop();

uint32_t media_cache_token();
void media_cache_observe(uint32_t token, void (*on_ready)(const std::string &path));
void media_cache_unobserve(uint32_t token);

std::shared_ptr<const MediaEntry> media_cache_lookup(const std::string &path);
std::shared_ptr<const CoverPixels> media_cache_image(const std::string &path, int32_t side);
void media_cache_request(const std::string &path, uint8_t want, int32_t side,
                         MetaPriority priority, uint32_t token);
std::shared_ptr<const MediaEntry> media_cache_resolve(const std::string &path, uint8_t want,
                                                      int32_t side, uint32_t timeout_ms);
void media_cache_cancel(uint32_t token);
void media_cache_idle_cancel();
void media_cache_forget(const std::string &mount_point);
void media_cache_invalidate_decoded();
void media_cache_register_harness();
