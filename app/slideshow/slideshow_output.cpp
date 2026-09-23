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

#include <algorithm>
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

bsp_size_t SlideshowOutput::screen() const {
    const bool swap = rotation_ == BSP_ROTATION_90 || rotation_ == BSP_ROTATION_270;
    return swap ? bsp_size_t{ panel_.height, panel_.width } : panel_;
}

bsp_rect_t SlideshowOutput::to_panel(bsp_rect_t rect) const {
    const bsp_size_t size = screen();
    const int x = rect.origin.x;
    const int y = rect.origin.y;
    const int w = rect.size.width;
    const int h = rect.size.height;
    switch (rotation_) {
    case BSP_ROTATION_90: return { { y, size.width - x - w }, { h, w } };
    case BSP_ROTATION_180: return { { size.width - x - w, size.height - y - h }, { w, h } };
    case BSP_ROTATION_270: return { { size.height - y - h, x }, { h, w } };
    default: return rect;
    }
}

static bsp_rect_t intersect(bsp_rect_t a, bsp_rect_t b) {
    const int left = std::max(a.origin.x, b.origin.x);
    const int top = std::max(a.origin.y, b.origin.y);
    const int right = std::min(a.origin.x + a.size.width, b.origin.x + b.size.width);
    const int bottom = std::min(a.origin.y + a.size.height, b.origin.y + b.size.height);
    if (right <= left || bottom <= top) return {};
    return { { left, top }, { right - left, bottom - top } };
}

bool SlideshowOutput::place(const ImagePixels &pixels, Placement *out) const {
    const bsp_size_t size = screen();
    if (!pixels.data || !pixels.width || pixels.width > size.width || !pixels.height ||
        pixels.height > size.height) {
        return false;
    }
    out->pixels = &pixels;
    out->rect = { { (size.width - pixels.width) / 2, (size.height - pixels.height) / 2 },
                  { pixels.width, pixels.height } };
    return true;
}

bool SlideshowOutput::compose(uint8_t *frame, const Placement &placement) {
    return draw_region(frame, placement, { { 0, 0 }, screen() });
}

bool SlideshowOutput::draw_region(uint8_t *frame, const Placement &placement,
                                  bsp_rect_t region) {
    const ImagePixels *pixels = placement.pixels;
    if (!frame || !pixels) return false;
    region = intersect(region, { { 0, 0 }, screen() });
    if (!region.size.width) return true;

    const bsp_rect_t image = intersect(region, placement.rect);
    if (!image.size.width) {
        fill_black(frame, to_panel(region));
        return true;
    }
    const int region_right = region.origin.x + region.size.width;
    const int region_bottom = region.origin.y + region.size.height;
    const int image_right = image.origin.x + image.size.width;
    const int image_bottom = image.origin.y + image.size.height;
    const bsp_rect_t borders[] = {
        { region.origin, { region.size.width, image.origin.y - region.origin.y } },
        { { region.origin.x, image_bottom }, { region.size.width, region_bottom - image_bottom } },
        { { region.origin.x, image.origin.y }, { image.origin.x - region.origin.x, image.size.height } },
        { { image_right, image.origin.y }, { region_right - image_right, image.size.height } },
    };
    for (const bsp_rect_t &border : borders) {
        if (border.size.width > 0 && border.size.height > 0) fill_black(frame, to_panel(border));
    }

    const bsp_rect_t out = to_panel(image);
    ppa_srm_oper_config_t op = {};
    op.in.buffer = pixels->data;
    op.in.pic_w = pixels->width;
    op.in.pic_h = pixels->height;
    op.in.block_offset_x = image.origin.x - placement.rect.origin.x;
    op.in.block_offset_y = image.origin.y - placement.rect.origin.y;
    op.in.block_w = image.size.width;
    op.in.block_h = image.size.height;
    op.in.srm_cm = pixels->rgb888 ? PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = frame;
    op.out.buffer_size = (uint32_t)frame_bytes();
    op.out.pic_w = panel_.width;
    op.out.pic_h = panel_.height;
    op.out.block_offset_x = out.origin.x;
    op.out.block_offset_y = out.origin.y;
    op.out.srm_cm = srm_mode_;
    op.rotation_angle = ppa_rotation(rotation_);
    op.scale_x = 1.0f;
    op.scale_y = 1.0f;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t err = ppa_do_scale_rotate_mirror(srm_, &op);
    if (err == ESP_OK) return true;
    ESP_LOGW(TAG, "draw: %s", esp_err_to_name(err));
    return false;
}

bool SlideshowOutput::copy(uint8_t *out, const uint8_t *in) {
    return copy_region(out, { 0, 0 }, in, { { 0, 0 }, screen() });
}

bool SlideshowOutput::copy_region(uint8_t *out, bsp_point_t to, const uint8_t *in,
                                  bsp_rect_t from) {
    if (!out || !in) return false;
    if (from.size.width <= 0 || from.size.height <= 0) return true;
    const bsp_rect_t source = to_panel(from);
    const bsp_rect_t target = to_panel({ to, from.size });
    ppa_srm_oper_config_t op = {};
    op.in.buffer = in;
    op.in.pic_w = panel_.width;
    op.in.pic_h = panel_.height;
    op.in.block_offset_x = source.origin.x;
    op.in.block_offset_y = source.origin.y;
    op.in.block_w = source.size.width;
    op.in.block_h = source.size.height;
    op.in.srm_cm = srm_mode_;
    op.out.buffer = out;
    op.out.buffer_size = (uint32_t)frame_bytes();
    op.out.pic_w = panel_.width;
    op.out.pic_h = panel_.height;
    op.out.block_offset_x = target.origin.x;
    op.out.block_offset_y = target.origin.y;
    op.out.srm_cm = srm_mode_;
    op.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    op.scale_x = 1.0f;
    op.scale_y = 1.0f;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t err = ppa_do_scale_rotate_mirror(srm_, &op);
    if (err == ESP_OK) return true;
    ESP_LOGW(TAG, "copy: %s", esp_err_to_name(err));
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
