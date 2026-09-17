/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "video_renderer.hpp"
#include "driver/ppa.h"
#include "h264_dec.h"

class H264Renderer : public VideoRenderer {
public:
    ~H264Renderer() override { close(); }

    bool open(const SharedSram &sram, bsp_pixel_format_t format, const TrackInfo &track,
              std::string *error) override;
    void close() override;
    bool pipelined() const override { return true; }

    DecodeResult decode(const uint8_t *data, std::size_t len, VideoPresenterRelease release,
                        void *ctx, bool present, VideoFrame *frame, std::string *error) override;
    bool draw(VideoFrame *frame, const RenderTarget &target, std::string *error) override;
    void drop(VideoFrame *frame) override;
    void discard() override;
    void restart() override;
    bool has_picture() const override { return held_ >= 0; }

private:
    static constexpr int kPictureSlots = 32;

    h264_dec_t *decoder_ = nullptr;
    ppa_client_handle_t ppa_ = nullptr;
    ppa_srm_color_mode_t color_mode_ = PPA_SRM_COLOR_MODE_RGB565;
    uint8_t nal_length_size_ = 0;
    h264_dec_picture_t pictures_[kPictureSlots] = {};
    int held_ = -1;
    bool reported_ = false;
};
