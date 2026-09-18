/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_renderer.hpp"
#include "h264_threads.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "h264_renderer";

static constexpr uint32_t kMaxMacroblocks = 3600;
static constexpr uint32_t kMaxSide = 1280;
static constexpr uint8_t kHeldPictures = 4;
static constexpr std::size_t kFrameBudgetBytes = 14 * 1024 * 1024;
static constexpr std::size_t kFrameAlignment = 64;

static void *alloc_psram(void *, std::size_t bytes) {
    return heap_caps_aligned_alloc(kFrameAlignment, bytes, MALLOC_CAP_SPIRAM);
}

static void free_psram(void *, void *ptr) {
    heap_caps_free(ptr);
}

static ppa_srm_rotation_angle_t ppa_rotation(bsp_rotation_t rotation) {
    switch (rotation) {
    case BSP_ROTATION_90:  return PPA_SRM_ROTATION_ANGLE_90;
    case BSP_ROTATION_180: return PPA_SRM_ROTATION_ANGLE_180;
    case BSP_ROTATION_270: return PPA_SRM_ROTATION_ANGLE_270;
    default:               return PPA_SRM_ROTATION_ANGLE_0;
    }
}

static bool uses_bt709(const h264_dec_stream_info_t &info) {
    switch (info.matrix_coefficients) {
    case 1: return true;
    case 5:
    case 6: return false;
    default: return info.height >= 720;
    }
}

static std::string describe(h264_dec_t *decoder, h264_dec_result_t result) {
    const char *message = h264_dec_error(decoder);
    if (message) return message;
    switch (result) {
    case H264_DEC_NO_MEMORY: return "not enough memory for H.264";
    case H264_DEC_NO_FRAME: return "H.264 decoder ran out of frames";
    default: return "H.264 decode failed";
    }
}

bool H264Renderer::open(const SharedSram &sram, bsp_pixel_format_t format, const TrackInfo &track,
                        std::string *error) {
    close();
    switch (format) {
    case BSP_PIXEL_FORMAT_RGB565: color_mode_ = PPA_SRM_COLOR_MODE_RGB565; break;
    case BSP_PIXEL_FORMAT_RGB888: color_mode_ = PPA_SRM_COLOR_MODE_RGB888; break;
    default:
        *error = "unsupported panel pixel format for video";
        return false;
    }

    if (!track.codec_private.empty()) {
        h264_dec_stream_info_t info = {};
        const char *failure = nullptr;
        if (!h264_dec_probe(track.codec_private.data(), track.codec_private.size(), 0, &info,
                            &failure) && failure) {
            *error = failure;
            return false;
        }
    }

    h264_dec_config_t config = {};
    config.alloc = alloc_psram;
    config.free = free_psram;
    config.work = static_cast<uint8_t *>(sram.base);
    config.work_bytes = sram.bytes;
    config.max_mbs = kMaxMacroblocks;
    config.max_side = kMaxSide;
    config.held_pictures = kHeldPictures;
    config.frame_budget_bytes = kFrameBudgetBytes;
    config.threads = h264_threads();
    decoder_ = h264_dec_create(&config);
    if (!decoder_) {
        *error = "H.264 decoder unavailable";
        return false;
    }
    nal_length_size_ = track.nal_length_size;
    if (!track.codec_private.empty()) {
        h264_dec_decode(decoder_, track.codec_private.data(), track.codec_private.size(), 0, 0);
    }

    ppa_client_config_t client = {};
    client.oper_type = PPA_OPERATION_SRM;
    client.max_pending_trans_num = 1;
    const esp_err_t err = ppa_register_client(&client, &ppa_);
    if (err != ESP_OK) {
        ppa_ = nullptr;
        close();
        *error = std::string("video scaler unavailable: ") + esp_err_to_name(err);
        return false;
    }
    reported_ = false;
    return true;
}

void H264Renderer::close() {
    discard();
    if (ppa_) {
        ppa_unregister_client(ppa_);
        ppa_ = nullptr;
    }
    if (decoder_) {
        h264_dec_destroy(decoder_);
        decoder_ = nullptr;
    }
}

DecodeResult H264Renderer::decode(const uint8_t *data, std::size_t len,
                                  VideoPresenterRelease release, void *ctx, bool present,
                                  int64_t due_us, VideoFrame *frame, std::string *error) {
    (void)frame;
    const int64_t tag = present ? due_us : kHiddenTag;
    const h264_dec_result_t result = decoder_
        ? h264_dec_decode(decoder_, data, len, nal_length_size_, tag)
        : H264_DEC_UNSUPPORTED;
    if (release) release(ctx);

    switch (result) {
    case H264_DEC_OK:
    case H264_DEC_NO_PICTURE:
    case H264_DEC_BAD_DATA:
        return DecodeResult::Hidden;
    default:
        *error = decoder_ ? describe(decoder_, result) : "video decoder is not open";
        return DecodeResult::Failed;
    }
}

bool H264Renderer::take(VideoFrame *frame, int64_t *due_us) {
    if (!decoder_) return false;
    h264_dec_picture_t picture = {};
    while (h264_dec_output(decoder_, &picture)) {
        if (!reported_) {
            reported_ = true;
            const h264_dec_stream_info_t &info = picture.info;
            ESP_LOGI(TAG, "%ux%u (coded %ux%u) profile %u level %u refs %u %s range %s",
                     info.width, info.height, info.coded_width, info.coded_height,
                     info.profile_idc, info.level_idc, info.max_ref_frames,
                     info.full_range ? "full" : "limited",
                     uses_bt709(info) ? "BT.709" : "BT.601");
        }
        if (picture.tag == kHiddenTag || picture.id >= kPictureSlots) {
            h264_dec_release(decoder_, picture.id);
            continue;
        }
        pictures_[picture.id] = picture;
        *frame = {};
        frame->id = picture.id;
        frame->size = { picture.info.width, picture.info.height };
        *due_us = picture.tag;
        return true;
    }
    return false;
}

void H264Renderer::drain() {
    if (decoder_) h264_dec_drain(decoder_);
}

bool H264Renderer::draw(VideoFrame *frame, const RenderTarget &target, std::string *error) {
    const int id = frame ? frame->id : held_;
    if (id < 0) return false;
    const h264_dec_picture_t &picture = pictures_[id];
    const h264_dec_stream_info_t &info = picture.info;

    ppa_srm_oper_config_t op = {};
    op.in.buffer = picture.packed;
    op.in.pic_w = info.coded_width;
    op.in.pic_h = info.coded_height;
    op.in.block_w = info.width;
    op.in.block_h = info.height;
    op.in.block_offset_x = info.crop_left;
    op.in.block_offset_y = info.crop_top;
    op.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
    op.in.yuv_range = info.full_range ? PPA_COLOR_RANGE_FULL : PPA_COLOR_RANGE_LIMIT;
    op.in.yuv_std = uses_bt709(info) ? PPA_COLOR_CONV_STD_RGB_YUV_BT709
                                     : PPA_COLOR_CONV_STD_RGB_YUV_BT601;
    op.out.buffer = target.framebuffer;
    op.out.buffer_size = target.framebuffer_bytes;
    op.out.pic_w = (uint32_t)target.panel.width;
    op.out.pic_h = (uint32_t)target.panel.height;
    op.out.block_offset_x = (uint32_t)target.rect.origin.x;
    op.out.block_offset_y = (uint32_t)target.rect.origin.y;
    op.out.srm_cm = color_mode_;
    op.rotation_angle = ppa_rotation(target.rotation);
    op.scale_x = (float)target.scale_n / kScaleDenominator;
    op.scale_y = op.scale_x;
    op.mode = PPA_TRANS_MODE_BLOCKING;

    const esp_err_t err = ppa_ ? ppa_do_scale_rotate_mirror(ppa_, &op) : ESP_ERR_INVALID_STATE;
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa: %s", esp_err_to_name(err));
        *error = std::string("video scaling failed: ") + esp_err_to_name(err);
        if (frame) drop(frame);
        return false;
    }
    if (frame) {
        discard();
        held_ = frame->id;
        frame->id = -1;
    }
    return true;
}

void H264Renderer::drop(VideoFrame *frame) {
    if (frame->id >= 0 && decoder_) h264_dec_release(decoder_, (uint8_t)frame->id);
    frame->id = -1;
}

void H264Renderer::discard() {
    if (held_ >= 0 && decoder_) h264_dec_release(decoder_, (uint8_t)held_);
    held_ = -1;
}

void H264Renderer::restart() {
    if (decoder_) h264_dec_flush(decoder_);
}
