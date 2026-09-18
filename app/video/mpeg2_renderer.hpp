/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstdint>
#include "video_renderer.hpp"
#include "packed_yuv_scaler.hpp"
#include "mpeg2_dec.h"

class Mpeg2Renderer : public VideoRenderer {
public:
    ~Mpeg2Renderer() override { close(); }

    bool open(const SharedSram &sram, bsp_pixel_format_t format, const TrackInfo &track,
              std::string *error) override;
    void close() override;
    bool pipelined() const override { return true; }

    DecodeResult decode(const uint8_t *data, std::size_t len, VideoPresenterRelease release,
                        void *ctx, bool present, int64_t due_us, VideoFrame *frame,
                        std::string *error) override;
    bool take(VideoFrame *frame, int64_t *due_us) override;
    void drain() override;
    bool draw(VideoFrame *frame, const RenderTarget &target, std::string *error) override;
    void drop(VideoFrame *frame) override;
    void discard() override;
    void restart() override;
    bool has_picture() const override { return held_ >= 0; }

private:
    static constexpr int kPictureSlots = 32;
    static constexpr int64_t kHiddenTag = INT64_MIN;

    mpeg2_dec_t *decoder_ = nullptr;
    PackedYuvScaler scaler_;
    vdec_threads_t threads_ = {};
    mpeg2_dec_picture_t pictures_[kPictureSlots] = {};
    int held_ = -1;
    bool reported_ = false;
};
