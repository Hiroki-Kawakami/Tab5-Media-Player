/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "render_target.hpp"
#include "media_player.hpp"
#include "jpeg_ppa_pipeline.h"
#include <string>

class MjpegRenderer {
public:
    bool open(const SharedSram &sram, bsp_pixel_format_t format, std::string *error);
    void close();

    bool probe(const uint8_t *data, std::size_t len, bsp_size_t *size, std::string *error);
    bool render(const uint8_t *data, std::size_t len, VideoPresenterRelease release, void *ctx,
                const RenderTarget &target, std::string *error);
    bool rerender(const RenderTarget &target, std::string *error);
    void discard();
    bool has_picture() const { return held_.data != nullptr; }

private:
    enum class Path { None, Direct, Pipeline };

    struct Packet {
        const uint8_t *data = nullptr;
        std::size_t len = 0;
        VideoPresenterRelease release = nullptr;
        void *ctx = nullptr;
    };

    bool draw(const uint8_t *data, std::size_t len, const RenderTarget &target, std::string *error);
    bool decode_direct(const uint8_t *data, std::size_t len, const RenderTarget &target,
                       std::string *error);
    bool decode_scaled(const uint8_t *data, std::size_t len, const RenderTarget &target,
                       std::string *error);

    jpeg_ppa_pipeline_handle_t pipeline_ = nullptr;
    ppa_srm_color_mode_t color_mode_ = PPA_SRM_COLOR_MODE_RGB565;
    Packet held_;
    Path path_ = Path::None;
};
