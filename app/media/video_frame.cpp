/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "video_frame.hpp"
#include "h264_dec.h"
#include "mpeg2_dec.h"
#include "video/packed_yuv_scaler.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "video_frame";

static constexpr std::size_t kWorkBytes = 240 * 1024;
static constexpr uint32_t kMaxMacroblocks = 3600;
static constexpr uint32_t kMaxSide = 1280;
static constexpr std::size_t kFrameAlignment = 64;

static void *alloc_psram(void *, std::size_t bytes) {
    return heap_caps_aligned_alloc(kFrameAlignment, bytes, MALLOC_CAP_SPIRAM);
}

static void free_psram(void *, void *ptr) {
    heap_caps_free(ptr);
}

bool video_frame_decodable(CodecId codec) {
    return codec == CodecId::Mjpeg || codec == CodecId::H264 || codec == CodecId::Mpeg2;
}

/* bsp_rotation_t turns counter-clockwise. */
ImageOrientation video_frame_orientation(bsp_rotation_t rotation) {
    switch (rotation) {
    case BSP_ROTATION_90: return ImageOrientation::Rot270;
    case BSP_ROTATION_180: return ImageOrientation::Rot180;
    case BSP_ROTATION_270: return ImageOrientation::Rot90;
    default: return ImageOrientation::None;
    }
}

static bool h264_bt709(const h264_dec_stream_info_t &info) {
    switch (info.matrix_coefficients) {
    case 1: return true;
    case 5:
    case 6: return false;
    default: return info.height >= 720;
    }
}

static bool mpeg2_bt709(const mpeg2_dec_stream_info_t &info) {
    switch (info.matrix_coefficients) {
    case 1: return true;
    case 5:
    case 6: return false;
    default: return info.height >= 720;
    }
}

static std::shared_ptr<ImagePixels> decode_h264(const VideoKeyframe &frame, uint8_t *work,
                                                ImageSize box, bool rgb888,
                                                ImageOrientation orientation) {
    h264_dec_config_t config = {};
    config.alloc = alloc_psram;
    config.free = free_psram;
    config.work = work;
    config.work_bytes = kWorkBytes;
    config.max_mbs = kMaxMacroblocks;
    config.max_side = kMaxSide;
    config.intra_only = true;
    h264_dec_t *decoder = h264_dec_create(&config);
    if (!decoder) {
        ESP_LOGE(TAG, "no H.264 decoder (psram largest %u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return nullptr;
    }
    if (!frame.config.empty()) {
        h264_dec_decode(decoder, frame.config.data(), frame.config.size(), 0, 0);
    }
    const h264_dec_result_t result = h264_dec_decode(decoder, frame.bytes->data(),
                                                     frame.bytes->size(), frame.nal_length_size, 0);
    h264_dec_drain(decoder);

    std::shared_ptr<ImagePixels> pixels;
    h264_dec_picture_t picture = {};
    if (h264_dec_output(decoder, &picture)) {
        const h264_dec_stream_info_t &info = picture.info;
        PackedYuvImage image;
        image.packed = picture.packed;
        image.coded_width = info.coded_width;
        image.coded_height = info.coded_height;
        image.crop_left = info.crop_left;
        image.crop_top = info.crop_top;
        image.width = info.width;
        image.height = info.height;
        image.full_range = info.full_range;
        image.bt709 = h264_bt709(info);
        pixels = image_from_packed_yuv(image, box, rgb888, orientation);
    } else {
        const char *error = h264_dec_error(decoder);
        ESP_LOGE(TAG, "no H.264 picture: result %d %s", (int)result, error ? error : "");
    }
    h264_dec_destroy(decoder);
    return pixels;
}

static std::shared_ptr<ImagePixels> decode_mpeg2(const VideoKeyframe &frame, uint8_t *work,
                                                 ImageSize box, bool rgb888,
                                                 ImageOrientation orientation) {
    mpeg2_dec_config_t config = {};
    config.alloc = alloc_psram;
    config.free = free_psram;
    config.work = work;
    config.work_bytes = kWorkBytes;
    config.max_mbs = kMaxMacroblocks;
    config.max_side = kMaxSide;
    config.intra_only = true;
    mpeg2_dec_t *decoder = mpeg2_dec_create(&config);
    if (!decoder) {
        ESP_LOGE(TAG, "no MPEG-2 decoder (psram largest %u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return nullptr;
    }
    if (!frame.config.empty()) mpeg2_dec_decode(decoder, frame.config.data(), frame.config.size(), 0);
    const mpeg2_dec_result_t result =
        mpeg2_dec_decode(decoder, frame.bytes->data(), frame.bytes->size(), 0);
    mpeg2_dec_drain(decoder);

    std::shared_ptr<ImagePixels> pixels;
    mpeg2_dec_picture_t picture = {};
    if (mpeg2_dec_output(decoder, &picture)) {
        const mpeg2_dec_stream_info_t &info = picture.info;
        PackedYuvImage image;
        image.packed = picture.packed;
        image.coded_width = info.coded_width;
        image.coded_height = info.coded_height;
        image.width = info.width;
        image.height = info.height;
        image.bt709 = mpeg2_bt709(info);
        pixels = image_from_packed_yuv(image, box, rgb888, orientation);
    } else {
        const char *error = mpeg2_dec_error(decoder);
        ESP_LOGE(TAG, "no MPEG-2 picture: result %d %s", (int)result, error ? error : "");
    }
    mpeg2_dec_destroy(decoder);
    return pixels;
}

std::shared_ptr<ImagePixels> video_frame_decode(const VideoKeyframe &frame, ImageSize box,
                                                bool rgb888, ImageOrientation orientation) {
    if (!frame.bytes || !box.valid()) return nullptr;
    auto *work = static_cast<uint8_t *>(
        heap_caps_aligned_alloc(kFrameAlignment, kWorkBytes, MALLOC_CAP_SPIRAM));
    if (!work) {
        ESP_LOGE(TAG, "no memory for the decoder's work arena (psram largest %u)",
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        return nullptr;
    }
    std::shared_ptr<ImagePixels> pixels;
    if (frame.codec == CodecId::H264) {
        pixels = decode_h264(frame, work, box, rgb888, orientation);
    } else if (frame.codec == CodecId::Mpeg2) {
        pixels = decode_mpeg2(frame, work, box, rgb888, orientation);
    }
    heap_caps_free(work);
    return pixels;
}
