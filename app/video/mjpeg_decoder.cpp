/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "mjpeg_decoder.hpp"
#include "jpeg_info.hpp"
#include "esp_log.h"

static const char *TAG = "mjpeg_decoder";

static constexpr uint32_t kMaxWidth = 1920;
static constexpr uint32_t kMaxHeight = 1088;
static constexpr uint32_t kMcuAlignment = 16;

static uint32_t align_up(uint32_t value, uint32_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

static bool output_format_for(bsp_pixel_format_t format, jpeg_dec_output_format_t *out) {
    switch (format) {
    case BSP_PIXEL_FORMAT_RGB565: *out = JPEG_DECODE_OUT_FORMAT_RGB565; return true;
    case BSP_PIXEL_FORMAT_RGB888: *out = JPEG_DECODE_OUT_FORMAT_RGB888; return true;
    default: return false;
    }
}

bool MjpegDecoder::open(bsp_pixel_format_t format, std::string *error) {
    if (decoder_ && format_ == format) return true;

    jpeg_dec_output_format_t output_format;
    if (!output_format_for(format, &output_format)) {
        *error = "unsupported panel pixel format for video";
        return false;
    }
    if (decoder_) {
        jpeg_enh_strip_decoder_del(decoder_);
        decoder_ = nullptr;
    }

    jpeg_enh_strip_decoder_cfg_t config = {};
    config.decode.output_format = output_format;
    config.decode.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR;
    config.decode.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    config.decode.yuv_full_range = true;
    config.max_pic_w = kMaxWidth;
    config.max_pic_h = kMaxHeight;
    const esp_err_t err = jpeg_enh_strip_decoder_new(&config, &decoder_);
    if (err != ESP_OK) {
        decoder_ = nullptr;
        ESP_LOGE(TAG, "jpeg_enh_strip_decoder_new: %s", esp_err_to_name(err));
        *error = std::string("video decoder unavailable: ") + esp_err_to_name(err);
        return false;
    }
    format_ = format;
    return true;
}

bool MjpegDecoder::decode(const uint8_t *data, std::size_t len, FrameAllocator &allocator,
                          DecodedFrame *out, std::string *error) {
    if (!decoder_) {
        *error = "video decoder is not open";
        return false;
    }

    uint32_t width = 0, height = 0;
    if (!jpeg_image_size(data, len, &width, &height, error)) return false;
    if (width > kMaxWidth || height > kMaxHeight) {
        *error = "video frame is too large to decode";
        return false;
    }

    const FrameBuffer buffer =
        allocator.lease(align_up(width, kMcuAlignment), align_up(height, kMcuAlignment));
    if (!buffer.data) {
        *error = "out of memory for the video decoder";
        return false;
    }

    jpeg_enh_frame_info_t info = {};
    const esp_err_t err = jpeg_enh_decoder_process(decoder_, data, (uint32_t)len,
                                                   buffer.data, buffer.capacity, &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "decode: %s", esp_err_to_name(err));
        *error = std::string("decode failed: ") + esp_err_to_name(err);
        return false;
    }

    out->pixels = buffer.data;
    out->pic_w = info.pic_w;
    out->pic_h = info.pic_h;
    out->width = info.origin_w;
    out->height = info.origin_h;
    return true;
}
