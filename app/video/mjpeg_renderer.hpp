/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "video_renderer.hpp"
#include "jpeg_ppa_pipeline.h"

class MjpegRenderer : public VideoRenderer {
public:
    ~MjpegRenderer() override { close(); }

    bool open(const SharedSram &sram, bsp_pixel_format_t format, const TrackInfo &track,
              std::string *error) override;
    void close() override;

    DecodeResult decode(const uint8_t *data, std::size_t len, VideoPresenterRelease release,
                        void *ctx, bool present, int64_t due_us, VideoFrame *frame,
                        std::string *error) override;
    bool draw(VideoFrame *frame, const RenderTarget &target, std::string *error) override;
    void drop(VideoFrame *frame) override;
    void discard() override;
    bool has_picture() const override { return held_.data != nullptr; }

private:
    enum class Path { None, Direct, Pipeline };

    bool decode_direct(const uint8_t *data, std::size_t len, const RenderTarget &target,
                       std::string *error);
    bool decode_scaled(const uint8_t *data, std::size_t len, const RenderTarget &target,
                       std::string *error);

    jpeg_ppa_pipeline_handle_t pipeline_ = nullptr;
    ppa_srm_color_mode_t color_mode_ = PPA_SRM_COLOR_MODE_RGB565;
    VideoFrame held_;
    Path path_ = Path::None;
};
