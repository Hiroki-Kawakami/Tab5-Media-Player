/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "render_target.hpp"
#include "media_player.hpp"
#include "media/media_types.hpp"
#include <string>

enum class DecodeResult {
    Failed,
    Hidden,
    Ready,
};

struct VideoFrame {
    bsp_size_t size = {};
    const uint8_t *data = nullptr;
    std::size_t len = 0;
    VideoPresenterRelease release = nullptr;
    void *ctx = nullptr;
    int id = -1;
};

class VideoRenderer {
public:
    virtual ~VideoRenderer() = default;

    virtual bool open(const SharedSram &sram, bsp_pixel_format_t format, const TrackInfo &track,
                      std::string *error) = 0;
    virtual void close() = 0;
    virtual bool pipelined() const { return false; }
    virtual bool needs_source() const { return false; }

    virtual DecodeResult decode(const uint8_t *data, std::size_t len, VideoPresenterRelease release,
                                void *ctx, bool present, int64_t due_us, VideoFrame *frame,
                                std::string *error) = 0;
    virtual bool take(VideoFrame *frame, int64_t *due_us) { (void)frame; (void)due_us; return false; }
    virtual void drain() {}
    virtual bool draw(VideoFrame *frame, const RenderTarget &target, std::string *error) = 0;
    virtual void drop(VideoFrame *frame) = 0;
    virtual void discard() = 0;
    virtual void restart() {}
    virtual bool has_picture() const = 0;
};
