/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "image_codec.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "imgf_alloc.h"
#include "imgf_decoder.h"
#include "imgf_resize.h"
#include "imgf_sniff.h"
#include "imgf_stream.h"
#include "jpeg_ppa_pipeline.h"
#include "video/jpeg_info.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

static const char *TAG = "image_codec";

static constexpr std::size_t kAlignment = 64;
static constexpr std::size_t kMaxFileBytes = 16 * 1024 * 1024;
static constexpr std::size_t kReadChunkBytes = 256 * 1024;
static constexpr uint32_t kScaleDenominator = 16;
static constexpr uint32_t kStripRows = 16;
static constexpr uint32_t kMaxHardwareWidth = 4096;
/* jpeg_new_*_engine() dereferences its half-built handle when it runs out of
   DMA-capable internal RAM (IDF v6.1), so the engine is only asked for when
   there is clearly room: the software path covers the rest. */
static constexpr std::size_t kEngineReserve = 8 * 1024;

static jpeg_ppa_pipeline_handle_t s_pipeline;
static uint8_t *s_strips[2];
static std::size_t s_strip_bytes;

namespace {

struct CancelSource {
    imgf_stream_t inner;
    const volatile bool *cancel;
};

int cancel_read(void *user, void *dst, std::size_t n) {
    auto *source = static_cast<CancelSource *>(user);
    if (source->cancel && *source->cancel) return -1;
    return source->inner.read(source->inner.user, dst, n);
}

imgf_stream_t with_cancel(CancelSource *state, imgf_stream_t inner, const volatile bool *cancel) {
    state->inner = inner;
    state->cancel = cancel;
    return { cancel_read, state };
}

}

ImagePixels::~ImagePixels() {
    heap_caps_free(data);
}

static std::size_t align_up(std::size_t value) {
    return (value + kAlignment - 1) / kAlignment * kAlignment;
}

static uint8_t *alloc_dma(std::size_t bytes) {
    return static_cast<uint8_t *>(
        heap_caps_aligned_alloc(kAlignment, align_up(bytes), MALLOC_CAP_SPIRAM));
}

static std::shared_ptr<ImagePixels> alloc_pixels(uint16_t width, uint16_t height, bool rgb888) {
    auto pixels = std::make_shared<ImagePixels>();
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

static imgf_resize_opts_t contain_opts(uint32_t src_w, uint32_t src_h, int32_t box_w, int32_t box_h,
                                       bool rgb888) {
    imgf_resize_opts_t opts = {};
    opts.target_w = (uint16_t)std::min<uint32_t>((uint32_t)box_w, src_w);
    opts.target_h = (uint16_t)std::min<uint32_t>((uint32_t)box_h, src_h);
    opts.fit = IMGF_FIT_CONTAIN;
    opts.dst_pixfmt = target_pixfmt(rgb888);
    opts.alloc_caps = MALLOC_CAP_SPIRAM;
    return opts;
}

static bool fit_inside(uint32_t src_w, uint32_t src_h, int32_t box_w, int32_t box_h,
                       uint16_t *dst_w, uint16_t *dst_h) {
    const imgf_resize_opts_t opts = contain_opts(src_w, src_h, box_w, box_h, false);
    return imgf_resize_compute_dst((uint16_t)src_w, (uint16_t)src_h, &opts, dst_w, dst_h) == IMGF_OK;
}

static bool engine_memory_available() {
#ifdef ESP_PLATFORM
    return heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) >= kEngineReserve;
#else
    return true;
#endif
}

static jpeg_ppa_pipeline_handle_t pipeline(std::size_t strip_bytes) {
    if (s_pipeline && s_strip_bytes >= strip_bytes) return s_pipeline;
    if (!engine_memory_available()) return nullptr;
    image_codec_close();

    for (uint8_t *&strip : s_strips) {
        strip = alloc_dma(strip_bytes);
        if (!strip) {
            image_codec_close();
            return nullptr;
        }
    }
    s_strip_bytes = strip_bytes;

    jpeg_ppa_pipeline_cfg_t cfg = {};
    cfg.strip_bufs[0] = s_strips[0];
    cfg.strip_bufs[1] = s_strips[1];
    cfg.strip_buf_size = strip_bytes;
    cfg.strip_color_mode = PPA_SRM_COLOR_MODE_RGB888;
    cfg.rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB;
    cfg.conv_std = JPEG_YUV_RGB_CONV_STD_BT601;
    cfg.yuv_full_range = true;
    cfg.timeout_ms = 5000;
    const esp_err_t err = jpeg_ppa_pipeline_new(&cfg, &s_pipeline);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no jpeg pipeline: %s", esp_err_to_name(err));
        s_pipeline = nullptr;
        image_codec_close();
    }
    return s_pipeline;
}

static uint32_t ppa_scale(uint32_t width, uint32_t height, uint16_t dst_w, uint16_t dst_h) {
    for (uint32_t scale = 1; scale < kScaleDenominator; scale++) {
        if (width * scale / kScaleDenominator >= dst_w &&
            height * scale / kScaleDenominator >= dst_h) {
            return scale;
        }
    }
    return kScaleDenominator;
}

static std::shared_ptr<ImagePixels> decode_hardware(const uint8_t *data, std::size_t size,
                                                    uint32_t width, uint32_t height,
                                                    uint16_t dst_w, uint16_t dst_h, bool rgb888) {
    if (width > kMaxHardwareWidth) return nullptr;
    const uint32_t padded = (width + kStripRows - 1) / kStripRows * kStripRows;
    jpeg_ppa_pipeline_handle_t handle = pipeline(kStripRows * padded * 3);
    if (!handle) return nullptr;

    const uint32_t scale = ppa_scale(width, height, dst_w, dst_h);
    const uint32_t mid_w = width * scale / kScaleDenominator;
    const uint32_t mid_h = height * scale / kScaleDenominator;
    if (!mid_w || !mid_h) return nullptr;

    const std::size_t mid_bytes = align_up((std::size_t)mid_w * mid_h * 3);
    uint8_t *mid = alloc_dma(mid_bytes);
    if (!mid) return nullptr;

    jpeg_ppa_output_t out = {};
    out.buffer = mid;
    out.buffer_size = mid_bytes;
    out.pic_w = mid_w;
    out.pic_h = mid_h;
    out.color_mode = PPA_SRM_COLOR_MODE_RGB888;

    jpeg_ppa_transform_t transform = {};
    transform.scale_x = (float)scale / kScaleDenominator;
    transform.scale_y = transform.scale_x;

    const esp_err_t err = jpeg_ppa_pipeline_process(handle, data, size, &out, &transform, nullptr);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "jpeg decode: %s", esp_err_to_name(err));
        heap_caps_free(mid);
        return nullptr;
    }

    imgf_resize_opts_t opts = {};
    opts.target_w = dst_w;
    opts.target_h = dst_h;
    opts.fit = IMGF_FIT_STRETCH;
    opts.dst_pixfmt = target_pixfmt(rgb888);
    opts.alloc_caps = MALLOC_CAP_SPIRAM;

    std::shared_ptr<ImagePixels> pixels = alloc_pixels(dst_w, dst_h, rgb888);
    if (pixels && imgf_resize_buffer(mid, (uint16_t)mid_w, (uint16_t)mid_h,
                                     (std::size_t)mid_w * 3, IMGF_PIX_RGB888, pixels->data,
                                     pixels->stride, &opts) != IMGF_OK) {
        pixels.reset();
    }
    heap_caps_free(mid);
    return pixels;
}

static std::shared_ptr<ImagePixels> decode_streaming(imgf_stream_t stream, imgf_format_t format,
                                                     int32_t box_w, int32_t box_h, bool rgb888,
                                                     const volatile bool *cancel, uint32_t *src_w_out,
                                                     uint32_t *src_h_out) {
    imgf_decoder_t *dec = imgf_make_decoder(format);
    if (!dec) return nullptr;

    imgf_decode_opts_t options = {};
    options.target_w = (uint16_t)box_w;
    options.target_h = (uint16_t)box_h;
    options.alloc_caps = MALLOC_CAP_SPIRAM;
    if (imgf_decoder_open(dec, stream, &options) != IMGF_OK) {
        imgf_decoder_destroy(dec);
        return nullptr;
    }

    const uint16_t src_w = imgf_decoder_width(dec);
    const uint16_t src_h = imgf_decoder_height(dec);
    const imgf_pixfmt_t src_pf = imgf_decoder_pixfmt(dec);
    if (src_w_out) *src_w_out = src_w;
    if (src_h_out) *src_h_out = src_h;
    const imgf_resize_opts_t opts = contain_opts(src_w, src_h, box_w, box_h, rgb888);

    imgf_err_t err = IMGF_OK;
    imgf_resizer_t *resizer = imgf_resizer_create(src_w, src_h, src_pf, &opts, &err);
    std::shared_ptr<ImagePixels> pixels;
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
        if (cancel && *cancel) {
            ok = false;
            break;
        }
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

static uint8_t *read_file(FILE *fp, std::size_t size, const volatile bool *cancel) {
    uint8_t *buffer = alloc_dma(size);
    if (!buffer) return nullptr;
    for (std::size_t done = 0; done < size;) {
        if (cancel && *cancel) {
            heap_caps_free(buffer);
            return nullptr;
        }
        const std::size_t want = std::min(kReadChunkBytes, size - done);
        if (fread(buffer + done, 1, want, fp) != want) {
            heap_caps_free(buffer);
            return nullptr;
        }
        done += want;
    }
    return buffer;
}

static ImageFormat format_of(imgf_format_t format) {
    switch (format) {
    case IMGF_FMT_JPEG: return ImageFormat::Jpeg;
    case IMGF_FMT_PNG: return ImageFormat::Png;
    default: return ImageFormat::Unknown;
    }
}

ImageLoad image_decode_file(const std::string &path, int32_t box_w, int32_t box_h, bool rgb888,
                            const volatile bool *cancel) {
    ImageLoad load;
    if (box_w <= 0 || box_h <= 0) {
        load.error = "invalid display size";
        return load;
    }

    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) {
        load.error = "cannot open the file";
        return load;
    }
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    rewind(fp);
    if (size <= 0) {
        fclose(fp);
        load.error = "empty file";
        return load;
    }
    load.file_bytes = size;

    uint8_t *buffer = nullptr;
    if ((std::size_t)size <= kMaxFileBytes) {
        buffer = read_file(fp, (std::size_t)size, cancel);
        if (!buffer) {
            fclose(fp);
            load.cancelled = cancel && *cancel;
            load.error = load.cancelled ? "" : "cannot read the file";
            return load;
        }
    }

    uint8_t header[8] = {};
    if (buffer) {
        memcpy(header, buffer, std::min(sizeof(header), (std::size_t)size));
    } else if (fread(header, 1, sizeof(header), fp) != sizeof(header)) {
        fclose(fp);
        load.error = "cannot read the file";
        return load;
    } else {
        rewind(fp);
    }

    const imgf_format_t format = imgf_sniff(header, sizeof(header));
    load.format = format_of(format);
    if (format == IMGF_FMT_UNKNOWN) {
        heap_caps_free(buffer);
        fclose(fp);
        load.error = "unsupported image format";
        return load;
    }

    if (buffer && format == IMGF_FMT_JPEG) {
        uint32_t width = 0;
        uint32_t height = 0;
        std::string unsupported;
        if (jpeg_image_size(buffer, (std::size_t)size, &width, &height, &unsupported)) {
            load.source_width = width;
            load.source_height = height;
            uint16_t dst_w = 0;
            uint16_t dst_h = 0;
            if (!(cancel && *cancel) && fit_inside(width, height, box_w, box_h, &dst_w, &dst_h)) {
                load.pixels = decode_hardware(buffer, (std::size_t)size, width, height, dst_w,
                                              dst_h, rgb888);
                load.hardware = load.pixels != nullptr;
            }
        }
    }

    if (!load.pixels && !(cancel && *cancel)) {
        imgf_buffer_source_t memory;
        imgf_file_source_t file;
        const imgf_stream_t source = buffer
            ? imgf_stream_from_buffer(&memory, buffer, (std::size_t)size)
            : imgf_stream_from_file(&file, fp, 0, 0);
        CancelSource state;
        uint32_t src_w = 0;
        uint32_t src_h = 0;
        load.pixels = decode_streaming(with_cancel(&state, source, cancel), format, box_w, box_h,
                                       rgb888, cancel, &src_w, &src_h);
        if (!load.source_width) {
            load.source_width = src_w;
            load.source_height = src_h;
        }
    }

    heap_caps_free(buffer);
    fclose(fp);
    if (!load.pixels) {
        load.cancelled = cancel && *cancel;
        load.error = load.cancelled ? "" : "cannot decode the image";
    }
    return load;
}

void image_codec_close() {
    if (s_pipeline) {
        jpeg_ppa_pipeline_del(s_pipeline);
        s_pipeline = nullptr;
    }
    for (uint8_t *&strip : s_strips) {
        heap_caps_free(strip);
        strip = nullptr;
    }
    s_strip_bytes = 0;
}
