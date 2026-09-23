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
#include "driver/ppa.h"
#include "jpeg_ppa_pipeline.h"
#ifdef ESP_PLATFORM
#include "driver/jpeg_encode.h"
#else
#include "imgf_encoder.h"
#include "imgf_jpege.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

static const char *TAG = "image_codec";

static constexpr int kQuality = 90;
static constexpr std::size_t kAlignment = 64;
static constexpr std::size_t kMaxFileBytes = 8 * 1024 * 1024;
static constexpr std::size_t kReadChunkBytes = 256 * 1024;
static constexpr std::size_t kHeaderWindowBytes = 128 * 1024;
static constexpr uint32_t kScaleDenominator = 16;
static constexpr uint32_t kStripRows = 16;
static constexpr uint32_t kMaxHardwareWidth = 4096;
static constexpr std::size_t kStripStepBytes = 64 * 1024;
static constexpr int32_t kDirectShortfallMax = 20;
/* jpeg_new_*_engine() dereferences its half-built handle when it runs out of
   DMA-capable internal RAM (IDF v6.1), so the engines are only asked for when
   there is clearly room: the software path covers the rest. */
static constexpr std::size_t kEngineReserve = 8 * 1024;

static jpeg_ppa_pipeline_handle_t s_pipeline;
static uint8_t *s_strips[2];
static std::size_t s_strip_bytes;
static ppa_client_handle_t s_srm;
#ifdef ESP_PLATFORM
static jpeg_encoder_handle_t s_encoder;
#endif

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

bool stopped(const volatile bool *cancel) {
    return cancel && *cancel;
}

}

static unsigned psram_free() {
#ifdef ESP_PLATFORM
    return (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
    return 0;
#endif
}

static unsigned psram_largest() {
    return (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
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
    if (pixels->data) return pixels;
    ESP_LOGE(TAG, "no memory for %ux%u pixels (%u bytes, psram free %u largest %u)",
             width, height, (unsigned)pixels->bytes, psram_free(), psram_largest());
    return nullptr;
}

static imgf_pixfmt_t target_pixfmt(bool rgb888) {
    return rgb888 ? IMGF_PIX_BGR888 : IMGF_PIX_RGB565;
}

static imgf_resize_opts_t contain_opts(uint32_t src_w, uint32_t src_h, ImageSize box, bool rgb888) {
    imgf_resize_opts_t opts = {};
    opts.target_w = (uint16_t)std::min<uint32_t>((uint32_t)box.width, src_w);
    opts.target_h = (uint16_t)std::min<uint32_t>((uint32_t)box.height, src_h);
    opts.fit = IMGF_FIT_CONTAIN;
    opts.dst_pixfmt = target_pixfmt(rgb888);
    opts.alloc_caps = MALLOC_CAP_SPIRAM;
    return opts;
}

static bool fit_inside(uint32_t src_w, uint32_t src_h, ImageSize box, uint16_t *dst_w,
                       uint16_t *dst_h) {
    const imgf_resize_opts_t opts = contain_opts(src_w, src_h, box, false);
    return imgf_resize_compute_dst((uint16_t)src_w, (uint16_t)src_h, &opts, dst_w, dst_h) == IMGF_OK;
}

static bool jpeg_header(const uint8_t *data, std::size_t size, ImageHeader *out) {
    std::size_t i = 2;
    while (i + 4 <= size) {
        if (data[i] != 0xFF) return false;
        const uint8_t marker = data[i + 1];
        if (marker == 0xFF) {
            i++;
            continue;
        }
        if (marker == 0x01 || (marker >= 0xD0 && marker <= 0xD9)) {
            i += 2;
            continue;
        }
        const std::size_t segment = ((std::size_t)data[i + 2] << 8) | data[i + 3];
        if (marker >= 0xC0 && marker <= 0xCF && marker != 0xC4 && marker != 0xC8 &&
            marker != 0xCC) {
            if (i + 9 > size) return false;
            out->height = ((uint32_t)data[i + 5] << 8) | data[i + 6];
            out->width = ((uint32_t)data[i + 7] << 8) | data[i + 8];
            out->hardware = marker == 0xC0 || marker == 0xC1;
            return out->width && out->height;
        }
        if (marker == 0xDA) return false;
        i += 2 + segment;
    }
    return false;
}

bool image_header(const uint8_t *data, std::size_t size, ImageHeader *out) {
    *out = {};
    switch (imgf_sniff(data, size)) {
    case IMGF_FMT_JPEG:
        out->format = ImageFormat::Jpeg;
        return jpeg_header(data, size, out);
    case IMGF_FMT_PNG:
        out->format = ImageFormat::Png;
        if (size < 24 || memcmp(data + 12, "IHDR", 4) != 0) return false;
        out->width = ((uint32_t)data[16] << 24) | ((uint32_t)data[17] << 16) |
                     ((uint32_t)data[18] << 8) | data[19];
        out->height = ((uint32_t)data[20] << 24) | ((uint32_t)data[21] << 16) |
                      ((uint32_t)data[22] << 8) | data[23];
        return out->width && out->height;
    default:
        return false;
    }
}

static bool engine_memory_available() {
#ifdef ESP_PLATFORM
    return heap_caps_get_largest_free_block(MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL) >= kEngineReserve;
#else
    return true;
#endif
}

static void release_pipeline() {
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

static jpeg_ppa_pipeline_handle_t pipeline(std::size_t strip_bytes) {
    strip_bytes = (strip_bytes + kStripStepBytes - 1) / kStripStepBytes * kStripStepBytes;
    if (s_pipeline && s_strip_bytes >= strip_bytes) return s_pipeline;
    if (!engine_memory_available()) {
        ESP_LOGW(TAG, "no internal memory for the jpeg engine");
        return nullptr;
    }
    release_pipeline();

    for (uint8_t *&strip : s_strips) {
        strip = alloc_dma(strip_bytes);
        if (!strip) {
            ESP_LOGW(TAG, "no memory for %u byte strips (psram free %u largest %u)",
                     (unsigned)strip_bytes, psram_free(), psram_largest());
            release_pipeline();
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
        release_pipeline();
    }
    return s_pipeline;
}

static ppa_srm_color_mode_t panel_color_mode(bool rgb888) {
    return rgb888 ? PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_RGB565;
}

/* How far short of the fitted size a picture may land before it is worth an
   intermediate and a pass of the box resizer to correct: five per cent of the
   box, capped so that a screen-sized picture cannot lose a visible border. */
static int32_t direct_shortfall_max(ImageSize box) {
    return std::min<int32_t>(box.longest() * 5 / 100, kDirectShortfallMax);
}

/* The largest sixteenth that does not overshoot the fitted size. Zero when the
   picture would still be too big at PPA's smallest step, which is every
   thumbnail of anything larger than sixteen times the box. */
static uint32_t ppa_fit_scale(uint32_t width, uint32_t height, uint16_t dst_w, uint16_t dst_h,
                              uint16_t *out_w, uint16_t *out_h) {
    const uint32_t scale = std::min((uint32_t)dst_w * kScaleDenominator / width,
                                    (uint32_t)dst_h * kScaleDenominator / height);
    if (scale < 1 || scale > kScaleDenominator) return 0;
    *out_w = (uint16_t)(width * scale / kScaleDenominator);
    *out_h = (uint16_t)(height * scale / kScaleDenominator);
    return (*out_w && *out_h) ? scale : 0;
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
                                                    const ImageHeader &header, ImageSize box,
                                                    bool rgb888) {
    if (header.width > kMaxHardwareWidth) {
        ESP_LOGI(TAG, "%ux%u is wider than the strip buffers allow", (unsigned)header.width,
                 (unsigned)header.height);
        return nullptr;
    }
    const uint32_t padded = (header.width + kStripRows - 1) / kStripRows * kStripRows;
    jpeg_ppa_pipeline_handle_t handle = pipeline(kStripRows * padded * 3);
    if (!handle) return nullptr;

    uint16_t dst_w = 0;
    uint16_t dst_h = 0;
    if (!fit_inside(header.width, header.height, box, &dst_w, &dst_h)) return nullptr;

    /* PPA scales in sixteenths, so it rarely lands on the fitted size, and the
       intermediate below exists to let the box resizer correct the rest. When
       the sixteenth under the fitted size is close enough, none of that is
       worth it: PPA writes the picture straight into its final buffer, in the
       panel's own format, and neither the intermediate nor a pass of the CPU
       over every pixel happens at all. The strips stay RGB888 in the decoder's
       R,G,B order, so the swap PPA does on the way in is what makes the output
       match what LVGL reads. */
    uint16_t fit_w = 0;
    uint16_t fit_h = 0;
    if (const uint32_t direct = ppa_fit_scale(header.width, header.height, dst_w, dst_h, &fit_w,
                                              &fit_h)) {
        const int32_t slack = direct_shortfall_max(box);
        if (dst_w - fit_w <= slack && dst_h - fit_h <= slack) {
            auto pixels = alloc_pixels(fit_w, fit_h, rgb888);
            if (!pixels) return nullptr;

            jpeg_ppa_output_t out = {};
            out.buffer = pixels->data;
            out.buffer_size = align_up(pixels->bytes);
            out.pic_w = fit_w;
            out.pic_h = fit_h;
            out.color_mode = panel_color_mode(rgb888);

            jpeg_ppa_transform_t transform = {};
            transform.scale_x = (float)direct / kScaleDenominator;
            transform.scale_y = transform.scale_x;
            transform.rgb_swap = true;

            const esp_err_t err =
                jpeg_ppa_pipeline_process(handle, data, size, &out, &transform, nullptr);
            if (err == ESP_OK) return pixels;
            ESP_LOGW(TAG, "jpeg decode: %s", esp_err_to_name(err));
            return nullptr;
        }
    }

    const uint32_t scale = ppa_scale(header.width, header.height, dst_w, dst_h);
    const uint32_t mid_w = header.width * scale / kScaleDenominator;
    const uint32_t mid_h = header.height * scale / kScaleDenominator;
    if (!mid_w || !mid_h) return nullptr;

    const std::size_t mid_bytes = align_up((std::size_t)mid_w * mid_h * 3);
    uint8_t *mid = alloc_dma(mid_bytes);
    if (!mid) {
        ESP_LOGW(TAG, "no memory for a %ux%u intermediate (%u bytes, psram free %u largest %u)",
                 (unsigned)mid_w, (unsigned)mid_h, (unsigned)mid_bytes, psram_free(),
                 psram_largest());
        return nullptr;
    }

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
        ESP_LOGW(TAG, "resize %ux%u -> %ux%u failed", (unsigned)mid_w, (unsigned)mid_h, dst_w,
                 dst_h);
        pixels.reset();
    }
    heap_caps_free(mid);
    return pixels;
}

static std::shared_ptr<ImagePixels> decode_streaming(imgf_stream_t stream, ImageFormat format,
                                                     ImageSize box, bool rgb888,
                                                     const volatile bool *cancel, bool *oom) {
    imgf_decoder_t *dec =
        imgf_make_decoder(format == ImageFormat::Png ? IMGF_FMT_PNG : IMGF_FMT_JPEG);
    if (!dec) {
        ESP_LOGE(TAG, "no decoder for format %d", (int)format);
        return nullptr;
    }

    imgf_decode_opts_t options = {};
    options.target_w = (uint16_t)box.width;
    options.target_h = (uint16_t)box.height;
    options.alloc_caps = MALLOC_CAP_SPIRAM;
    const imgf_err_t open_err = imgf_decoder_open(dec, stream, &options);
    if (open_err != IMGF_OK) {
        if (open_err == IMGF_ERR_OOM && oom) *oom = true;
        if (!stopped(cancel)) ESP_LOGE(TAG, "decoder open failed: imgf error %d", (int)open_err);
        imgf_decoder_destroy(dec);
        return nullptr;
    }

    const uint16_t src_w = imgf_decoder_width(dec);
    const uint16_t src_h = imgf_decoder_height(dec);
    const imgf_pixfmt_t src_pf = imgf_decoder_pixfmt(dec);
    const imgf_resize_opts_t opts = contain_opts(src_w, src_h, box, rgb888);

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
        if (oom && (!pixels || !row || err == IMGF_ERR_OOM)) *oom = true;
        if (!resizer) ESP_LOGE(TAG, "no resizer for %ux%u: imgf error %d", src_w, src_h, (int)err);
        if (resizer && !row) {
            ESP_LOGE(TAG, "no memory for a %u byte row (psram free %u largest %u)",
                     (unsigned)((std::size_t)src_w * imgf_pixfmt_bpp(src_pf)), psram_free(),
                     psram_largest());
        }
        imgf_free(row);
        imgf_resizer_destroy(resizer);
        imgf_decoder_destroy(dec);
        return nullptr;
    }

    uint32_t written = 0;
    bool ok = true;
    for (uint16_t y = 0; y < src_h && ok; y++) {
        if (stopped(cancel)) {
            ok = false;
            break;
        }
        if (!imgf_decoder_next_row(dec, row)) {
            const imgf_err_t row_err = imgf_decoder_last_error(dec);
            if (row_err == IMGF_ERR_OOM && oom) *oom = true;
            ESP_LOGE(TAG, "decode stopped at row %u of %u: imgf error %d", y, src_h, (int)row_err);
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
    if (!ok || written == 0) {
        if (ok && !stopped(cancel)) ESP_LOGE(TAG, "decoder produced no rows");
        return nullptr;
    }
    while (written < pixels->height) {
        memcpy(pixels->data + (std::size_t)written * pixels->stride,
               pixels->data + (std::size_t)(written - 1) * pixels->stride, pixels->stride);
        written++;
    }
    return pixels;
}

std::shared_ptr<ImagePixels> image_decode(const uint8_t *data, std::size_t size, ImageSize box,
                                          bool rgb888, const volatile bool *cancel,
                                          ImageNotes *notes) {
    if (!data || size == 0 || !box.valid() || stopped(cancel)) return nullptr;

    ImageHeader header;
    const bool parsed = image_header(data, size, &header);
    if (notes) notes->header = header;
    if (!parsed) {
        if (header.format == ImageFormat::Unknown) {
            ESP_LOGE(TAG, "not a JPEG or PNG (%u bytes)", (unsigned)size);
            return nullptr;
        }
        ESP_LOGW(TAG, "no usable header in a %u byte picture", (unsigned)size);
        header.width = 0;
    }
    if (header.hardware && header.width) {
        if (auto pixels = decode_hardware(data, size, header, box, rgb888)) return pixels;
    }
    if (stopped(cancel)) return nullptr;

    imgf_buffer_source_t memory;
    CancelSource state;
    bool oom = false;
    auto pixels = decode_streaming(with_cancel(&state, imgf_stream_from_buffer(&memory, data, size),
                                               cancel), header.format, box, rgb888, cancel, &oom);
    if (notes) notes->out_of_memory = oom;
    if (!pixels && !stopped(cancel)) {
        ESP_LOGE(TAG, "no pixels from a %ux%u %s (%u bytes, psram free %u largest %u)",
                 (unsigned)header.width, (unsigned)header.height,
                 header.format == ImageFormat::Png ? "PNG"
                 : header.hardware                 ? "baseline JPEG"
                                                   : "JPEG that is not baseline",
                 (unsigned)size, psram_free(), psram_largest());
    }
    return pixels;
}

static uint8_t *read_file(FILE *fp, std::size_t size, const volatile bool *cancel) {
    uint8_t *buffer = alloc_dma(size);
    if (!buffer) {
        ESP_LOGW(TAG, "no contiguous %u bytes to read into (psram free %u largest %u)",
                 (unsigned)size, psram_free(), psram_largest());
        return nullptr;
    }
    for (std::size_t done = 0; done < size;) {
        const std::size_t want = std::min(kReadChunkBytes, size - done);
        if (stopped(cancel) || fread(buffer + done, 1, want, fp) != want) {
            heap_caps_free(buffer);
            return nullptr;
        }
        done += want;
    }
    return buffer;
}

std::shared_ptr<ImagePixels> image_decode_file(const std::string &path, ImageSize box, bool rgb888,
                                               const volatile bool *cancel, ImageNotes *notes) {
    if (!box.valid()) return nullptr;
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return nullptr;
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    rewind(fp);
    if (size <= 0) {
        fclose(fp);
        return nullptr;
    }

    ImageHeader header;
    const std::size_t window = std::min((std::size_t)size, kHeaderWindowBytes);
    if (uint8_t *head = read_file(fp, window, cancel)) {
        image_header(head, window, &header);
        heap_caps_free(head);
    }
    if (notes) notes->header = header;
    if (header.format == ImageFormat::Unknown || stopped(cancel)) {
        fclose(fp);
        return nullptr;
    }

    /* Only the hardware decoder needs the file as one contiguous block, and it
       only takes baseline JPEG. Everything else is read a row at a time, which
       asks nothing of the heap and leaves the big blocks to whoever does. */
    const bool whole = header.format == ImageFormat::Jpeg &&
                       (std::size_t)size <= kMaxFileBytes &&
                       (!header.width || header.hardware);
    std::shared_ptr<ImagePixels> pixels;
    if (whole) {
        rewind(fp);
        if (uint8_t *buffer = read_file(fp, (std::size_t)size, cancel)) {
            pixels = image_decode(buffer, (std::size_t)size, box, rgb888, cancel, notes);
            heap_caps_free(buffer);
            fclose(fp);
            return pixels;
        }
        if (stopped(cancel)) {
            fclose(fp);
            return nullptr;
        }
    }

    rewind(fp);
    imgf_file_source_t file;
    CancelSource state;
    bool oom = false;
    pixels = decode_streaming(with_cancel(&state, imgf_stream_from_file(&file, fp, 0, 0), cancel),
                              header.format, box, rgb888, cancel, &oom);
    if (notes) notes->out_of_memory = oom;
    fclose(fp);
    return pixels;
}

#ifdef ESP_PLATFORM

static jpeg_encoder_handle_t encoder() {
    if (s_encoder) return s_encoder;
    if (!engine_memory_available()) return nullptr;

    jpeg_encode_engine_cfg_t cfg = {};
    cfg.timeout_ms = 5000;
    const esp_err_t err = jpeg_new_encoder_engine(&cfg, &s_encoder);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "no jpeg encoder: %s", esp_err_to_name(err));
        s_encoder = nullptr;
    }
    return s_encoder;
}

bool image_encode(const ImagePixels &pixels, PsramVector<uint8_t> *out) {
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

bool image_encode(const ImagePixels &pixels, PsramVector<uint8_t> *out) {
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

std::shared_ptr<ImagePixels> image_scale(const ImagePixels &src, ImageSize box) {
    if (!src.data || !box.valid()) return nullptr;

    const uint32_t scale = std::min((uint32_t)box.width * kScaleDenominator / src.width,
                                    (uint32_t)box.height * kScaleDenominator / src.height);
    if (scale < 1 || scale > 16 * kScaleDenominator) return nullptr;
    const uint16_t dst_w = (uint16_t)(src.width * scale / kScaleDenominator);
    const uint16_t dst_h = (uint16_t)(src.height * scale / kScaleDenominator);
    if (!dst_w || !dst_h) return nullptr;

    if (!s_srm) {
        ppa_client_config_t client = {};
        client.oper_type = PPA_OPERATION_SRM;
        client.max_pending_trans_num = 1;
        const esp_err_t err = ppa_register_client(&client, &s_srm);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "no ppa client: %s", esp_err_to_name(err));
            s_srm = nullptr;
            return nullptr;
        }
    }

    auto pixels = alloc_pixels(dst_w, dst_h, src.rgb888);
    if (!pixels) return nullptr;

    const ppa_srm_color_mode_t mode =
        src.rgb888 ? PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_RGB565;
    ppa_srm_oper_config_t op = {};
    op.in.buffer = src.data;
    op.in.pic_w = src.width;
    op.in.pic_h = src.height;
    op.in.block_w = src.width;
    op.in.block_h = src.height;
    op.in.srm_cm = mode;
    op.out.buffer = pixels->data;
    op.out.buffer_size = align_up(pixels->bytes);
    op.out.pic_w = dst_w;
    op.out.pic_h = dst_h;
    op.out.srm_cm = mode;
    op.scale_x = (float)scale / kScaleDenominator;
    op.scale_y = op.scale_x;
    op.mode = PPA_TRANS_MODE_BLOCKING;

    const esp_err_t err = ppa_do_scale_rotate_mirror(s_srm, &op);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "ppa scale %ux%u -> %ux%u: %s", (unsigned)src.width, (unsigned)src.height,
                 (unsigned)dst_w, (unsigned)dst_h, esp_err_to_name(err));
        return nullptr;
    }
    return pixels;
}

void image_codec_close() {
    release_pipeline();
    if (s_srm) {
        ppa_unregister_client(s_srm);
        s_srm = nullptr;
    }
#ifdef ESP_PLATFORM
    if (s_encoder) {
        jpeg_del_encoder_engine(s_encoder);
        s_encoder = nullptr;
    }
#endif
}
