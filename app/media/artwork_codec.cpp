/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "artwork_codec.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "imgf_alloc.h"
#include "imgf_decoder.h"
#include "imgf_resize.h"
#include "imgf_sniff.h"
#include "imgf_stream.h"
#include "jpeg_decode_enhanced.h"
#ifdef ESP_PLATFORM
#include "driver/jpeg_encode.h"
#else
#include "imgf_encoder.h"
#include "imgf_jpege.h"
#endif

#include <cstdlib>
#include <cstring>

static const char *TAG = "artwork";

static constexpr uint32_t kMaxWholeFramePixels = 1500 * 1000;
static constexpr uint32_t kMaxSourcePixels = 4096 * 4096;
static constexpr int kQuality = 90;
static constexpr std::size_t kAlignment = 64;
/* jpeg_new_*_engine() dereferences its half-built handle when it runs out of
   DMA-capable internal RAM (IDF v6.1), so the engines are only asked for when
   there is clearly room: the software path covers the rest. */
static constexpr std::size_t kEngineReserve = 8 * 1024;

static jpeg_enh_strip_decoder_handle_t s_decoder;
#ifdef ESP_PLATFORM
static jpeg_encoder_handle_t s_encoder;
#endif

CoverPixels::~CoverPixels() {
    heap_caps_free(data);
}

static std::size_t align_up(std::size_t value) {
    return (value + kAlignment - 1) / kAlignment * kAlignment;
}

static uint8_t *alloc_dma(std::size_t bytes) {
    return static_cast<uint8_t *>(
        heap_caps_aligned_alloc(kAlignment, align_up(bytes), MALLOC_CAP_SPIRAM));
}

static std::shared_ptr<CoverPixels> alloc_pixels(uint16_t width, uint16_t height, bool rgb888) {
    auto pixels = std::make_shared<CoverPixels>();
    pixels->width = width;
    pixels->height = height;
    pixels->rgb888 = rgb888;
    pixels->stride = (uint32_t)width * (rgb888 ? 3 : 2);
    pixels->bytes = (std::size_t)pixels->stride * height;
    pixels->data = alloc_dma(pixels->bytes);
    return pixels->data ? pixels : nullptr;
}

static imgf_pixfmt_t target_pixfmt(bool rgb888) {
    return rgb888 ? IMGF_PIX_BGR888 : IMGF_PIX_RGB565;
}

static imgf_resize_opts_t resize_opts(int32_t side, bool rgb888) {
    imgf_resize_opts_t opts = {};
    opts.target_w = (uint16_t)side;
    opts.target_h = (uint16_t)side;
    opts.fit = IMGF_FIT_CONTAIN;
    opts.dst_pixfmt = target_pixfmt(rgb888);
    opts.alloc_caps = MALLOC_CAP_SPIRAM;
    return opts;
}

static bool engine_memory_available() {
#ifdef ESP_PLATFORM
    return heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) >= kEngineReserve;
#else
    return true;
#endif
}

static jpeg_enh_strip_decoder_handle_t decoder() {
    if (s_decoder) return s_decoder;
    if (!engine_memory_available()) return nullptr;

    jpeg_enh_strip_decoder_cfg_t cfg = {};
    cfg.decode.output_format = JPEG_DECODE_OUT_FORMAT_RGB888;
    cfg.decode.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
    cfg.decode.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    cfg.decode.yuv_full_range = true;
    cfg.timeout_ms = 2000;
    const esp_err_t err = jpeg_enh_strip_decoder_new(&cfg, &s_decoder);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no jpeg decoder: %s", esp_err_to_name(err));
        s_decoder = nullptr;
    }
    return s_decoder;
}

static bool source_size(const uint8_t *data, std::size_t size, imgf_format_t format,
                        uint16_t *width, uint16_t *height) {
    imgf_decoder_t *dec = imgf_make_decoder(format);
    if (!dec) return false;

    imgf_buffer_source_t source;
    const imgf_stream_t stream = imgf_stream_from_buffer(&source, data, size);
    imgf_decode_opts_t options = {};
    options.max_src_pixels = kMaxSourcePixels;
    options.alloc_caps = MALLOC_CAP_SPIRAM;

    const bool ok = imgf_decoder_open(dec, stream, &options) == IMGF_OK;
    if (ok) {
        *width = imgf_decoder_width(dec);
        *height = imgf_decoder_height(dec);
    }
    imgf_decoder_destroy(dec);
    return ok;
}

static std::shared_ptr<CoverPixels> decode_whole_frame(const uint8_t *data, std::size_t size,
                                                       uint16_t width, uint16_t height,
                                                       int32_t side, bool rgb888) {
    jpeg_enh_strip_decoder_handle_t handle = decoder();
    if (!handle) return nullptr;

    const std::size_t padded_w = ((std::size_t)width + 15) / 16 * 16;
    const std::size_t padded_h = ((std::size_t)height + 15) / 16 * 16;
    const std::size_t bytes = padded_w * padded_h * 3;
    uint8_t *full = alloc_dma(bytes);
    if (!full) return nullptr;

    jpeg_enh_frame_info_t info = {};
    const esp_err_t err =
        jpeg_enh_decoder_process(handle, data, (uint32_t)size, full, align_up(bytes), &info);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg decode: %s", esp_err_to_name(err));
        heap_caps_free(full);
        return nullptr;
    }

    const imgf_resize_opts_t opts = resize_opts(side, rgb888);
    uint16_t dst_w = 0;
    uint16_t dst_h = 0;
    std::shared_ptr<CoverPixels> pixels;
    if (imgf_resize_compute_dst(info.origin_w, info.origin_h, &opts, &dst_w, &dst_h) == IMGF_OK) {
        pixels = alloc_pixels(dst_w, dst_h, rgb888);
    }
    if (pixels &&
        imgf_resize_buffer(full, info.origin_w, info.origin_h, info.pic_w * 3, IMGF_PIX_RGB888,
                           pixels->data, pixels->stride, &opts) != IMGF_OK) {
        pixels.reset();
    }
    heap_caps_free(full);
    return pixels;
}

static std::shared_ptr<CoverPixels> decode_streaming(const uint8_t *data, std::size_t size,
                                                     imgf_format_t format, int32_t side,
                                                     bool rgb888) {
    imgf_decoder_t *dec = imgf_make_decoder(format);
    if (!dec) return nullptr;

    imgf_buffer_source_t source;
    const imgf_stream_t stream = imgf_stream_from_buffer(&source, data, size);
    imgf_decode_opts_t options = {};
    options.target_w = (uint16_t)side;
    options.target_h = (uint16_t)side;
    options.max_src_pixels = kMaxSourcePixels;
    options.alloc_caps = MALLOC_CAP_SPIRAM;
    if (imgf_decoder_open(dec, stream, &options) != IMGF_OK) {
        imgf_decoder_destroy(dec);
        return nullptr;
    }

    const uint16_t src_w = imgf_decoder_width(dec);
    const uint16_t src_h = imgf_decoder_height(dec);
    const imgf_pixfmt_t src_pf = imgf_decoder_pixfmt(dec);
    const imgf_resize_opts_t opts = resize_opts(side, rgb888);

    imgf_err_t err = IMGF_OK;
    imgf_resizer_t *resizer = imgf_resizer_create(src_w, src_h, src_pf, &opts, &err);
    std::shared_ptr<CoverPixels> pixels;
    uint8_t *row = nullptr;
    if (resizer) {
        pixels = alloc_pixels(imgf_resizer_dst_width(resizer), imgf_resizer_dst_height(resizer),
                              rgb888);
        row = static_cast<uint8_t *>(
            imgf_alloc((std::size_t)src_w * imgf_pixfmt_bpp(src_pf), MALLOC_CAP_SPIRAM));
    }
    if (!resizer || !pixels || !row) {
        imgf_free(row);
        imgf_resizer_destroy(resizer);
        imgf_decoder_destroy(dec);
        return nullptr;
    }

    uint32_t written = 0;
    bool ok = true;
    for (uint16_t y = 0; y < src_h && ok; y++) {
        if (!imgf_decoder_next_row(dec, row)) {
            ok = false;
            break;
        }
        if (imgf_resizer_push_row(resizer, row) < 0) ok = false;
        while (ok && written < pixels->height &&
               imgf_resizer_pop_row(resizer, pixels->data + (std::size_t)written * pixels->stride)) {
            written++;
        }
    }
    if (ok && written < pixels->height && imgf_resizer_finish(resizer) > 0) {
        if (imgf_resizer_pop_row(resizer, pixels->data + (std::size_t)written * pixels->stride)) {
            written++;
        }
    }

    imgf_free(row);
    imgf_resizer_destroy(resizer);
    imgf_decoder_destroy(dec);
    if (!ok || written == 0) return nullptr;
    while (written < pixels->height) {
        memcpy(pixels->data + (std::size_t)written * pixels->stride,
               pixels->data + (std::size_t)(written - 1) * pixels->stride, pixels->stride);
        written++;
    }
    return pixels;
}

std::shared_ptr<CoverPixels> artwork_decode(const uint8_t *data, std::size_t size, int32_t side,
                                            bool rgb888) {
    if (!data || size == 0 || side <= 0) return nullptr;

    const imgf_format_t format = imgf_sniff(data, size);
    if (format == IMGF_FMT_UNKNOWN) return nullptr;

    if (format == IMGF_FMT_JPEG) {
        uint16_t width = 0;
        uint16_t height = 0;
        if (source_size(data, size, format, &width, &height) &&
            (uint32_t)width * height <= kMaxWholeFramePixels) {
            auto pixels = decode_whole_frame(data, size, width, height, side, rgb888);
            if (pixels) return pixels;
        }
    }
    return decode_streaming(data, size, format, side, rgb888);
}

#ifdef ESP_PLATFORM

static jpeg_encoder_handle_t encoder() {
    if (s_encoder) return s_encoder;
    if (!engine_memory_available()) return nullptr;

    jpeg_encode_engine_cfg_t cfg = {};
    cfg.timeout_ms = 2000;
    const esp_err_t err = jpeg_new_encoder_engine(&cfg, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no jpeg encoder: %s", esp_err_to_name(err));
        s_encoder = nullptr;
    }
    return s_encoder;
}

bool artwork_encode(const CoverPixels &pixels, PsramVector<uint8_t> *out) {
    jpeg_encoder_handle_t handle = encoder();
    if (!handle) return false;

    const std::size_t capacity = align_up((std::size_t)pixels.width * pixels.height + 8192);
    uint8_t *buffer = alloc_dma(capacity);
    if (!buffer) return false;

    jpeg_encode_cfg_t cfg = {};
    cfg.width = pixels.width;
    cfg.height = pixels.height;
    cfg.src_type = pixels.rgb888 ? JPEG_ENCODE_IN_FORMAT_RGB888 : JPEG_ENCODE_IN_FORMAT_RGB565;
    cfg.sub_sample = JPEG_DOWN_SAMPLING_YUV420;
    cfg.image_quality = kQuality;

    uint32_t written = 0;
    const esp_err_t err = jpeg_encoder_process(handle, &cfg, pixels.data, (uint32_t)pixels.bytes,
                                               buffer, (uint32_t)capacity, &written);
    if (err == ESP_OK && written > 0) out->assign(buffer, buffer + written);
    heap_caps_free(buffer);
    if (err != ESP_OK) ESP_LOGW(TAG, "jpeg encode: %s", esp_err_to_name(err));
    return err == ESP_OK && written > 0;
}

#else

bool artwork_encode(const CoverPixels &pixels, PsramVector<uint8_t> *out) {
    imgf_jpege_opts_t opts = {};
    opts.quality = kQuality;
    opts.subsample = IMGF_JPEG_SUBSAMPLE_420;

    imgf_err_t err = IMGF_OK;
    imgf_encoder_t *encoder = imgf_jpege_create(pixels.width, pixels.height,
                                                pixels.rgb888 ? IMGF_PIX_BGR888 : IMGF_PIX_RGB565,
                                                &opts, &err);
    if (!encoder) return false;

    out->resize(imgf_encoder_buffer_size(encoder));
    bool ok = imgf_encoder_bind_buffer(encoder, out->data(), out->size()) == IMGF_OK;
    for (uint16_t y = 0; ok && y < pixels.height; y++) {
        ok = imgf_encoder_push_row(encoder, pixels.data + (std::size_t)y * pixels.stride) == 1;
    }
    std::size_t written = 0;
    ok = ok && imgf_encoder_finish(encoder, &written) == IMGF_OK && written > 0;
    imgf_encoder_destroy(encoder);
    if (!ok) {
        out->clear();
        return false;
    }
    out->resize(written);
    out->shrink_to_fit();
    return true;
}

#endif

void artwork_codec_close() {
    if (s_decoder) {
        jpeg_enh_strip_decoder_del(s_decoder);
        s_decoder = nullptr;
    }
#ifdef ESP_PLATFORM
    if (s_encoder) {
        jpeg_del_encoder_engine(s_encoder);
        s_encoder = nullptr;
    }
#endif
}
