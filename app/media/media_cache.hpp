/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/image_codec.hpp"
#include "media/image_exif.hpp"
#include "media/media_types.hpp"
#include "media_buffer.h"
#include "media_tags.h"
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

struct MediaEntry {
    bool ok = false;
    bool has_cover = false;
    /* Zero until the picture bytes have been seen: an info-only probe knows
       there is one and where it is, but not what it hashes to. */
    uint64_t image_id = 0;
    CoverLocation cover_at;
    bool cover_scanned = false;
    int64_t duration_us = 0;
    int64_t file_bytes = 0;
    CodecId audio_codec = CodecId::None;
    uint32_t sample_rate = 0;
    uint32_t bitrate_bps = 0;
    uint8_t channels = 0;
    uint8_t bits = 0;
    /* An image file's own size, in pixels; zero for everything else. */
    uint16_t image_width = 0;
    uint16_t image_height = 0;
    ImageFormat image_format = ImageFormat::Unknown;
    bool image_baseline = false;
    bool image_failed = false;
    bool image_too_large = false;
    /* Only for an image file that carries tags, so an entry that has none pays
       a pointer. */
    std::shared_ptr<const ImageExif> image_exif;
    char title[MEDIA_TAG_TEXT_BYTES] = {};
    char artist[MEDIA_TAG_TEXT_BYTES] = {};
    char album[MEDIA_TAG_TEXT_BYTES] = {};
};

enum MetaWant : uint8_t {
    MetaWantInfo = 1,
    MetaWantThumbnail = 2,
    MetaWantImage = 4,
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
/* What a MetaWantThumbnail request produced: RGB565 pixels shared with the cache. */
std::shared_ptr<const ImagePixels> media_cache_thumbnail(const std::string &path, ImageSize box);
/* Writes what a MetaWantImage request produced into `dst` as packed rows in the
   panel's format. `dst` must be 64-byte aligned and hold box width x height
   pixels. False if nothing is ready for that box yet. */
bool media_cache_read_image(const std::string &path, ImageSize box, uint8_t *dst,
                            std::size_t capacity, ImageSize *size);
/* The store that keeps MetaWantImage pictures between reads. Without it every
   read needs a request of its own, and idle requests for pictures are dropped. */
bool media_cache_reserve_pictures();
void media_cache_release_pictures();
void media_cache_request(const std::string &path, uint8_t want, ImageSize box,
                         MetaPriority priority, uint32_t token);
std::shared_ptr<const MediaEntry> media_cache_resolve(const std::string &path, uint8_t want,
                                                      ImageSize box, uint32_t timeout_ms);
void media_cache_cancel(uint32_t token);
/* Keeps only the paths `keep` accepts: the rest of the token's requests are
   dropped and a running job the token owns is withdrawn. `keep` is called with
   the cache lock held, so it must not call back into the cache. */
void media_cache_retain(uint32_t token, bool (*keep)(const char *path, void *ctx), void *ctx);
void media_cache_idle_cancel();
void media_cache_forget(const std::string &mount_point);
void media_cache_register_harness();
