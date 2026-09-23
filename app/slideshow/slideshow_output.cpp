/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "slideshow_output.hpp"
#include "bsp.h"
#include "display_manager.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#ifdef ESP_PLATFORM
#include "esp_cache.h"
#endif

#include <cstring>
#include <initializer_list>

static const char *TAG = "slideshow";

static constexpr std::size_t kFrameAlignment = 64;

static ppa_srm_rotation_angle_t ppa_rotation(bsp_rotation_t rotation) {
    switch (rotation) {
    case BSP_ROTATION_90: return PPA_SRM_ROTATION_ANGLE_90;
    case BSP_ROTATION_180: return PPA_SRM_ROTATION_ANGLE_180;
    case BSP_ROTATION_270: return PPA_SRM_ROTATION_ANGLE_270;
    default: return PPA_SRM_ROTATION_ANGLE_0;
    }
}

static ppa_client_handle_t register_client(ppa_operation_t operation) {
    ppa_client_config_t config = {};
    config.oper_type = operation;
    config.max_pending_trans_num = 1;
    ppa_client_handle_t client = nullptr;
    const esp_err_t err = ppa_register_client(&config, &client);
    if (err == ESP_OK) return client;
    ESP_LOGE(TAG, "no ppa client: %s", esp_err_to_name(err));
    return nullptr;
}

bool SlideshowOutput::open(bsp_rotation_t rotation) {
    switch (bsp_display_get_pixel_format()) {
    case BSP_PIXEL_FORMAT_RGB565:
        srm_mode_ = PPA_SRM_COLOR_MODE_RGB565;
        blend_mode_ = PPA_BLEND_COLOR_MODE_RGB565;
        bytes_per_pixel_ = 2;
        break;
    case BSP_PIXEL_FORMAT_RGB888:
        srm_mode_ = PPA_SRM_COLOR_MODE_RGB888;
        blend_mode_ = PPA_BLEND_COLOR_MODE_RGB888;
        bytes_per_pixel_ = 3;
        break;
    default:
        return false;
    }
    for (int i = 0; i < kFramebuffers; i++) {
        if (!framebuffer(i)) {
            ESP_LOGE(TAG, "needs %d framebuffers", kFramebuffers);
            return false;
        }
    }
    srm_ = register_client(PPA_OPERATION_SRM);
    blend_ = register_client(PPA_OPERATION_BLEND);
    if (!srm_ || !blend_) {
        close();
        return false;
    }
    rotation_ = rotation;
    panel_ = bsp_display_get_size();
    shown_ = 0;
    presents_ = 1;
    for (uint32_t &presented : presented_) presented = 0;
    presented_[0] = presents_;
    return true;
}

void SlideshowOutput::close() {
    if (srm_) ppa_unregister_client(srm_);
    if (blend_) ppa_unregister_client(blend_);
    srm_ = nullptr;
    blend_ = nullptr;
}

void FrameDeleter::operator()(uint8_t *frame) const {
    heap_caps_free(frame);
}

uint8_t *SlideshowOutput::framebuffer(int index) const {
    return static_cast<uint8_t *>(bsp_display_get_frame_buffer(index));
}

std::size_t SlideshowOutput::frame_bytes() const {
    return (std::size_t)panel_.width * panel_.height * bytes_per_pixel_;
}

Frame SlideshowOutput::allocate_frame() const {
    const std::size_t bytes =
        (frame_bytes() + kFrameAlignment - 1) / kFrameAlignment * kFrameAlignment;
    auto *frame = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(kFrameAlignment, bytes, MALLOC_CAP_SPIRAM));
    if (!frame) ESP_LOGE(TAG, "no memory for a %u byte frame", (unsigned)bytes);
    return Frame(frame);
}

int SlideshowOutput::least_recent(int exclude, int also_exclude) const {
    int best = -1;
    for (int i = 0; i < kFramebuffers; i++) {
        if (i == exclude || i == also_exclude) continue;
        if (best < 0 || presented_[i] < presented_[best]) best = i;
    }
    return best;
}

void SlideshowOutput::present(int index) {
    display_manager.present(index);
    shown_ = index;
    presented_[index] = ++presents_;
}

bool SlideshowOutput::place(const ImagePixels &pixels, Placement *out) const {
    const bool swap = rotation_ == BSP_ROTATION_90 || rotation_ == BSP_ROTATION_270;
    const int width = swap ? pixels.height : pixels.width;
    const int height = swap ? pixels.width : pixels.height;
    if (!pixels.data || !width || width > panel_.width || !height || height > panel_.height) {
        return false;
    }
    out->pixels = &pixels;
    out->rect = { { (panel_.width - width) / 2, (panel_.height - height) / 2 }, { width, height } };
    return true;
}

bool SlideshowOutput::compose(uint8_t *target, const Placement &placement) {
    const ImagePixels *pixels = placement.pixels;
    if (!target || !pixels) return false;

    const bsp_rect_t &rect = placement.rect;
    const int right = rect.origin.x + rect.size.width;
    const int bottom = rect.origin.y + rect.size.height;
    fill_black(target, { { 0, 0 }, { panel_.width, rect.origin.y } });
    fill_black(target, { { 0, bottom }, { panel_.width, panel_.height - bottom } });
    fill_black(target, { { 0, rect.origin.y }, { rect.origin.x, rect.size.height } });
    fill_black(target, { { right, rect.origin.y }, { panel_.width - right, rect.size.height } });

    ppa_srm_oper_config_t op = {};
    op.in.buffer = pixels->data;
    op.in.pic_w = pixels->width;
    op.in.pic_h = pixels->height;
    op.in.block_w = pixels->width;
    op.in.block_h = pixels->height;
    op.in.srm_cm = pixels->rgb888 ? PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = target;
    op.out.buffer_size = (uint32_t)frame_bytes();
    op.out.pic_w = panel_.width;
    op.out.pic_h = panel_.height;
    op.out.block_offset_x = rect.origin.x;
    op.out.block_offset_y = rect.origin.y;
    op.out.srm_cm = srm_mode_;
    op.rotation_angle = ppa_rotation(rotation_);
    op.scale_x = 1.0f;
    op.scale_y = 1.0f;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t err = ppa_do_scale_rotate_mirror(srm_, &op);
    if (err == ESP_OK) return true;
    ESP_LOGW(TAG, "compose: %s", esp_err_to_name(err));
    return false;
}

bool SlideshowOutput::blend(uint8_t *out, const uint8_t *bg, const uint8_t *fg, uint8_t fg_alpha) {
    if (!out || !bg || !fg) return false;

    ppa_blend_oper_config_t op = {};
    for (ppa_in_pic_blk_config_t *in : { &op.in_bg, &op.in_fg }) {
        in->pic_w = panel_.width;
        in->pic_h = panel_.height;
        in->block_w = panel_.width;
        in->block_h = panel_.height;
        in->blend_cm = blend_mode_;
    }
    op.in_bg.buffer = bg;
    op.in_fg.buffer = fg;
    op.out.buffer = out;
    op.out.buffer_size = (uint32_t)frame_bytes();
    op.out.pic_w = panel_.width;
    op.out.pic_h = panel_.height;
    op.out.blend_cm = blend_mode_;
    op.bg_alpha_update_mode = PPA_ALPHA_NO_CHANGE;
    op.fg_alpha_update_mode = PPA_ALPHA_FIX_VALUE;
    op.fg_alpha_fix_val = fg_alpha;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t err = ppa_do_blend(blend_, &op);
    if (err == ESP_OK) return true;
    ESP_LOGW(TAG, "blend: %s", esp_err_to_name(err));
    return false;
}

void SlideshowOutput::fill_black(uint8_t *frame) {
    if (frame) fill_black(frame, { { 0, 0 }, panel_ });
}

void SlideshowOutput::fill_black(uint8_t *frame, bsp_rect_t area) const {
    if (area.size.width <= 0 || area.size.height <= 0) return;
    const std::size_t stride = (std::size_t)panel_.width * bytes_per_pixel_;
    const std::size_t row_bytes = (std::size_t)area.size.width * bytes_per_pixel_;
    const std::size_t first = (std::size_t)area.origin.y * stride +
                              (std::size_t)area.origin.x * bytes_per_pixel_;
    for (int row = 0; row < area.size.height; row++) {
        memset(frame + first + (std::size_t)row * stride, 0, row_bytes);
    }
#ifdef ESP_PLATFORM
    const std::size_t span = (std::size_t)(area.size.height - 1) * stride + row_bytes;
    esp_cache_msync(frame + first, span,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#endif
}
