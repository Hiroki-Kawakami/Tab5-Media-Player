/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "mpeg2_renderer.hpp"
#include "video_threads.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "mpeg2_renderer";

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

static bool uses_bt709(const mpeg2_dec_stream_info_t &info) {
    switch (info.matrix_coefficients) {
    case 1: return true;
    case 5:
    case 6: return false;
    default: return info.height >= 720;
    }
}

static std::string describe(mpeg2_dec_t *decoder, mpeg2_dec_result_t result) {
    const char *message = mpeg2_dec_error(decoder);
    if (message) return message;
    switch (result) {
    case MPEG2_DEC_NO_MEMORY: return "not enough memory for MPEG-2";
    case MPEG2_DEC_NO_FRAME: return "MPEG-2 decoder ran out of frames";
    default: return "MPEG-2 decode failed";
    }
}

bool Mpeg2Renderer::open(const SharedSram &sram, bsp_pixel_format_t format, const TrackInfo &track,
                         std::string *error) {
    close();
    if (!scaler_.open(format, error)) return false;

    if (!track.codec_private.empty()) {
        mpeg2_dec_stream_info_t info = {};
        const char *failure = nullptr;
        if (!mpeg2_dec_probe(track.codec_private.data(), track.codec_private.size(), &info,
                             &failure) && failure) {
            *error = failure;
            return false;
        }
    }

    mpeg2_dec_config_t config = {};
    config.alloc = alloc_psram;
    config.free = free_psram;
    config.work = static_cast<uint8_t *>(sram.base);
    config.work_bytes = sram.bytes;
    config.max_mbs = kMaxMacroblocks;
    config.max_side = kMaxSide;
    config.held_pictures = kHeldPictures;
    config.frame_budget_bytes = kFrameBudgetBytes;
    threads_ = video_threads("mpeg2_rows");
    config.threads = &threads_;
    decoder_ = mpeg2_dec_create(&config);
    if (!decoder_) {
        close();
        *error = "MPEG-2 decoder unavailable";
        return false;
    }
    if (!track.codec_private.empty()) {
        const mpeg2_dec_result_t result =
            mpeg2_dec_decode(decoder_, track.codec_private.data(), track.codec_private.size(), 0);
        if (result == MPEG2_DEC_UNSUPPORTED || result == MPEG2_DEC_NO_MEMORY) {
            *error = describe(decoder_, result);
            close();
            return false;
        }
    }
    reported_ = false;
    return true;
}

void Mpeg2Renderer::close() {
    discard();
    scaler_.close();
    if (decoder_) {
        mpeg2_dec_destroy(decoder_);
        decoder_ = nullptr;
    }
}

DecodeResult Mpeg2Renderer::decode(const uint8_t *data, std::size_t len,
                                   VideoPresenterRelease release, void *ctx, bool present,
                                   int64_t due_us, VideoFrame *frame, std::string *error) {
    (void)frame;
    const int64_t tag = present ? due_us : kHiddenTag;
    const mpeg2_dec_result_t result =
        decoder_ ? mpeg2_dec_decode(decoder_, data, len, tag) : MPEG2_DEC_UNSUPPORTED;
    if (release) release(ctx);

    switch (result) {
    case MPEG2_DEC_OK:
    case MPEG2_DEC_NO_PICTURE:
    case MPEG2_DEC_BAD_DATA:
        return DecodeResult::Hidden;
    default:
        *error = decoder_ ? describe(decoder_, result) : "video decoder is not open";
        return DecodeResult::Failed;
    }
}

bool Mpeg2Renderer::take(VideoFrame *frame, int64_t *due_us) {
    if (!decoder_) return false;
    mpeg2_dec_picture_t picture = {};
    while (mpeg2_dec_output(decoder_, &picture)) {
        if (!reported_) {
            reported_ = true;
            const mpeg2_dec_stream_info_t &info = picture.info;
            ESP_LOGI(TAG, "%ux%u (coded %ux%u) profile/level 0x%02x %s", info.width, info.height,
                     info.coded_width, info.coded_height, info.profile_and_level,
                     uses_bt709(info) ? "BT.709" : "BT.601");
        }
        if (picture.tag == kHiddenTag || picture.id >= kPictureSlots) {
            mpeg2_dec_release(decoder_, picture.id);
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

void Mpeg2Renderer::drain() {
    if (decoder_) mpeg2_dec_drain(decoder_);
}

bool Mpeg2Renderer::draw(VideoFrame *frame, const RenderTarget &target, std::string *error) {
    const int id = frame ? frame->id : held_;
    if (id < 0) return false;
    const mpeg2_dec_picture_t &picture = pictures_[id];
    const mpeg2_dec_stream_info_t &info = picture.info;

    PackedYuvImage image;
    image.packed = picture.packed;
    image.coded_width = info.coded_width;
    image.coded_height = info.coded_height;
    image.width = info.width;
    image.height = info.height;
    image.bt709 = uses_bt709(info);
    if (!scaler_.draw(image, target, error)) {
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

void Mpeg2Renderer::drop(VideoFrame *frame) {
    if (frame->id >= 0 && decoder_) mpeg2_dec_release(decoder_, (uint8_t)frame->id);
    frame->id = -1;
}

void Mpeg2Renderer::discard() {
    if (held_ >= 0 && decoder_) mpeg2_dec_release(decoder_, (uint8_t)held_);
    held_ = -1;
}

void Mpeg2Renderer::restart() {
    if (decoder_) mpeg2_dec_flush(decoder_);
}
