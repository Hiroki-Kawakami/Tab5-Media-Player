/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "bsp_types.h"
#include "driver/ppa.h"
#include "media/image_pixels.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>

struct FrameDeleter {
    void operator()(uint8_t *frame) const;
};
using Frame = std::unique_ptr<uint8_t, FrameDeleter>;

struct Placement {
    const ImagePixels *pixels = nullptr;
    bsp_rect_t rect = {};
};

/* The panel's framebuffers and the PPA operations a slideshow draws with. It
   never decides which framebuffer is used for what. */
class SlideshowOutput {
public:
    static constexpr int kFramebuffers = 3;

    bool open(bsp_rotation_t rotation);
    void close();

    bsp_rotation_t rotation() const { return rotation_; }
    bsp_size_t panel() const { return panel_; }
    bool rgb565() const { return bytes_per_pixel_ == 2; }
    uint8_t *framebuffer(int index) const;
    /* A panel-sized buffer in PSRAM that PPA can write. */
    Frame allocate_frame() const;
    int shown() const { return shown_; }
    /* The framebuffer presented longest ago, other than the two given. */
    int least_recent(int exclude, int also_exclude = -1) const;
    void present(int index);

    bool place(const ImagePixels &pixels, Placement *out) const;
    bool compose(uint8_t *frame, const Placement &placement);
    bool blend(uint8_t *out, const uint8_t *bg, const uint8_t *fg, uint8_t fg_alpha);
    void fill_black(uint8_t *frame);

private:
    void fill_black(uint8_t *frame, bsp_rect_t area) const;
    std::size_t frame_bytes() const;

    bsp_rotation_t rotation_ = BSP_ROTATION_0;
    bsp_size_t panel_ = {};
    uint8_t bytes_per_pixel_ = 0;
    ppa_srm_color_mode_t srm_mode_ = PPA_SRM_COLOR_MODE_RGB565;
    ppa_blend_color_mode_t blend_mode_ = PPA_BLEND_COLOR_MODE_RGB565;
    ppa_client_handle_t srm_ = nullptr;
    ppa_client_handle_t blend_ = nullptr;
    int shown_ = 0;
    uint32_t presented_[kFramebuffers] = {};
    uint32_t presents_ = 0;
};
