/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "mjpeg_renderer.hpp"
#include "jpeg_info.hpp"
#include "esp_log.h"

static const char *TAG = "mjpeg_renderer";

static constexpr uint32_t kStripRows = 16;
static constexpr uint32_t kMaxStripBytesPerPixel = 3;
static constexpr uint32_t kMaxWidth =
    kSharedSramBytes / 2 / (kStripRows * kMaxStripBytesPerPixel);
static constexpr uint32_t kMaxPixels = 1920 * 1088;
static constexpr uint32_t kMcuAlignment = 16;
static constexpr std::size_t kCacheAlignment = 64;
static constexpr uint32_t kDecodeTimeoutMs = 500;

static bool color_mode_for(bsp_pixel_format_t format, ppa_srm_color_mode_t *out) {
    switch (format) {
    case BSP_PIXEL_FORMAT_RGB565: *out = PPA_SRM_COLOR_MODE_RGB565; return true;
    case BSP_PIXEL_FORMAT_RGB888: *out = PPA_SRM_COLOR_MODE_RGB888; return true;
    default: return false;
    }
}

static ppa_srm_rotation_angle_t ppa_rotation(bsp_rotation_t rotation) {
    switch (rotation) {
    case BSP_ROTATION_90:  return PPA_SRM_ROTATION_ANGLE_90;
    case BSP_ROTATION_180: return PPA_SRM_ROTATION_ANGLE_180;
    case BSP_ROTATION_270: return PPA_SRM_ROTATION_ANGLE_270;
    default:               return PPA_SRM_ROTATION_ANGLE_0;
    }
}

static bool covers_panel_unscaled(const RenderTarget &target) {
    return target.rotation == BSP_ROTATION_0 &&
           target.scale_n == kScaleDenominator &&
           target.rect.origin.x == 0 && target.rect.origin.y == 0 &&
           target.rect.size.width == target.panel.width &&
           target.rect.size.height == target.panel.height &&
           target.source.width == target.panel.width &&
           target.source.height == target.panel.height &&
           target.source.width % kMcuAlignment == 0 &&
           target.source.height % kMcuAlignment == 0 &&
           (uintptr_t)target.framebuffer % kCacheAlignment == 0 &&
           target.framebuffer_bytes % kCacheAlignment == 0;
}

bool MjpegRenderer::open(const SharedSram &sram, bsp_pixel_format_t format, const TrackInfo &,
                         std::string *error) {
    close();
    if (!color_mode_for(format, &color_mode_)) {
        *error = "unsupported panel pixel format for video";
        return false;
    }

    jpeg_ppa_pipeline_cfg_t config = {};
    config.strip_bufs[0] = sram.halves[0];
    config.strip_bufs[1] = sram.halves[1];
    config.strip_buf_size = sram.half_bytes;
    config.strip_color_mode = color_mode_;
    config.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    config.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    config.yuv_full_range = true;
    config.timeout_ms = kDecodeTimeoutMs;
    path_ = Path::None;
    const esp_err_t err = jpeg_ppa_pipeline_new(&config, &pipeline_);
    if (err != ESP_OK) {
        pipeline_ = nullptr;
        ESP_LOGE(TAG, "jpeg_ppa_pipeline_new: %s", esp_err_to_name(err));
        *error = std::string("video decoder unavailable: ") + esp_err_to_name(err);
        return false;
    }
    return true;
}

void MjpegRenderer::close() {
    discard();
    if (pipeline_) {
        jpeg_ppa_pipeline_del(pipeline_);
        pipeline_ = nullptr;
    }
}

void MjpegRenderer::drop(VideoFrame *frame) {
    const VideoFrame f = *frame;
    *frame = {};
    if (f.release) f.release(f.ctx);
}

DecodeResult MjpegRenderer::decode(const uint8_t *data, std::size_t len,
                                   VideoPresenterRelease release, void *ctx, bool present,
                                   VideoFrame *frame, std::string *error) {
    VideoFrame packet;
    packet.data = data;
    packet.len = len;
    packet.release = release;
    packet.ctx = ctx;
    if (!present) {
        drop(&packet);
        return DecodeResult::Hidden;
    }
    uint32_t width = 0, height = 0;
    if (!jpeg_image_size(data, len, &width, &height, error)) {
        drop(&packet);
        return DecodeResult::Failed;
    }
    if (width > kMaxWidth) {
        *error = "video is wider than " + std::to_string(kMaxWidth) + " px";
        drop(&packet);
        return DecodeResult::Failed;
    }
    if ((uint64_t)width * height > kMaxPixels) {
        *error = "video frame is too large to decode";
        drop(&packet);
        return DecodeResult::Failed;
    }
    packet.size = { (int)width, (int)height };
    *frame = packet;
    return DecodeResult::Ready;
}

void MjpegRenderer::discard() {
    drop(&held_);
}

bool MjpegRenderer::draw(VideoFrame *frame, const RenderTarget &target, std::string *error) {
    const VideoFrame &source = frame ? *frame : held_;
    if (!source.data) return false;
    if (!pipeline_) {
        *error = "video decoder is not open";
        if (frame) drop(frame);
        return false;
    }
    const Path path = covers_panel_unscaled(target) ? Path::Direct : Path::Pipeline;
    if (path != path_) {
        path_ = path;
        ESP_LOGI(TAG, "%s: src %dx%d rotation %d scale %u/%u rect %d,%d %dx%d fb %p (%u bytes)",
                 path == Path::Direct ? "direct decode" : "pipeline",
                 target.source.width, target.source.height, (int)target.rotation,
                 (unsigned)target.scale_n, (unsigned)kScaleDenominator,
                 target.rect.origin.x, target.rect.origin.y,
                 target.rect.size.width, target.rect.size.height,
                 target.framebuffer, (unsigned)target.framebuffer_bytes);
    }
    const bool ok = path == Path::Direct ? decode_direct(source.data, source.len, target, error)
                                         : decode_scaled(source.data, source.len, target, error);
    if (!frame) return ok;
    if (!ok) {
        drop(frame);
        return false;
    }
    discard();
    held_ = *frame;
    *frame = {};
    return true;
}

bool MjpegRenderer::decode_direct(const uint8_t *data, std::size_t len,
                                  const RenderTarget &target, std::string *error) {
    jpeg_enh_frame_info_t info = {};
    const esp_err_t err = jpeg_enh_decoder_process(jpeg_ppa_pipeline_get_decoder(pipeline_),
                                                   data, (uint32_t)len, target.framebuffer,
                                                   target.framebuffer_bytes, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "direct decode: %s", esp_err_to_name(err));
        *error = std::string("decode failed: ") + esp_err_to_name(err);
        return false;
    }
    return true;
}

bool MjpegRenderer::decode_scaled(const uint8_t *data, std::size_t len,
                                  const RenderTarget &target, std::string *error) {
    jpeg_ppa_output_t output = {};
    output.buffer = target.framebuffer;
    output.buffer_size = target.framebuffer_bytes;
    output.pic_w = (uint32_t)target.panel.width;
    output.pic_h = (uint32_t)target.panel.height;
    output.color_mode = color_mode_;

    const float scale = (float)target.scale_n / kScaleDenominator;
    jpeg_ppa_transform_t transform = {};
    transform.rotation = ppa_rotation(target.rotation);
    transform.scale_x = scale;
    transform.scale_y = scale;
    transform.out_offset_x = (uint32_t)target.rect.origin.x;
    transform.out_offset_y = (uint32_t)target.rect.origin.y;

    const esp_err_t err = jpeg_ppa_pipeline_process(pipeline_, data, len, &output, &transform,
                                                    nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "pipeline: %s", esp_err_to_name(err));
        *error = std::string("decode failed: ") + esp_err_to_name(err);
        return false;
    }
    return true;
}
