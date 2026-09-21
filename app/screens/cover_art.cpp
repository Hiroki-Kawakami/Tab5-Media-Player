/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "cover_art.hpp"

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "imgf_alloc.h"
#include "imgf_decoder.h"
#include "imgf_resize.h"
#include "imgf_sniff.h"
#include "imgf_stream.h"

static const char *TAG = "cover_art";

static constexpr uint32_t kMaxSourcePixels = 4096 * 4096;

struct CoverImage {
    lv_image_dsc_t dsc;
    uint8_t *pixels;
};

static uint8_t *decode(const std::vector<uint8_t> &bytes, int32_t side, uint16_t *width,
                       uint16_t *height) {
    imgf_decoder_t *decoder = imgf_make_decoder(imgf_sniff(bytes.data(), bytes.size()));
    if (!decoder) return nullptr;

    imgf_buffer_source_t source;
    const imgf_stream_t stream = imgf_stream_from_buffer(&source, bytes.data(), bytes.size());
    imgf_decode_opts_t options = {};
    options.target_w = (uint16_t)side;
    options.target_h = (uint16_t)side;
    options.max_src_pixels = kMaxSourcePixels;
    options.alloc_caps = MALLOC_CAP_SPIRAM;

    uint8_t *pixels = nullptr;
    imgf_err_t err = imgf_decoder_open(decoder, stream, &options);
    if (err == IMGF_OK) {
        imgf_resize_opts_t resize = {};
        resize.target_w = (uint16_t)side;
        resize.target_h = (uint16_t)side;
        resize.fit = IMGF_FIT_CONTAIN;
        resize.dst_pixfmt = IMGF_PIX_RGB565;
        resize.alloc_caps = MALLOC_CAP_SPIRAM;
        err = imgf_resize_decoder(decoder, &resize, &pixels, width, height);
    }
    imgf_decoder_destroy(decoder);

    if (err != IMGF_OK) {
        ESP_LOGW(TAG, "cannot decode the artwork: %s", imgf_err_to_str(err));
        imgf_free(pixels);
        return nullptr;
    }
    return pixels;
}

lv_obj_t *cover_art_create(lv_obj_t *parent, const CoverArt &cover, int32_t side) {
    if (!cover || side <= 0) return nullptr;

    const int64_t started = esp_timer_get_time();
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t *pixels = decode(*cover.data, side, &width, &height);
    if (!pixels) return nullptr;
    ESP_LOGI(TAG, "%ux%u artwork from %u bytes in %lld ms", (unsigned)width, (unsigned)height,
             (unsigned)cover.data->size(), (long long)(esp_timer_get_time() - started) / 1000);

    auto image = new CoverImage{};
    image->pixels = pixels;
    image->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    image->dsc.header.cf = LV_COLOR_FORMAT_RGB565;
    image->dsc.header.w = width;
    image->dsc.header.h = height;
    image->dsc.header.stride = (uint32_t)width * 2;
    image->dsc.data = pixels;
    image->dsc.data_size = (uint32_t)width * height * 2;

    lv_obj_t *object = lv_image_create(parent);
    lv_image_set_src(object, &image->dsc);
    lv_obj_center(object);
    lv_obj_add_event_cb(object, [](lv_event_t *event) {
        auto stale = static_cast<CoverImage *>(lv_event_get_user_data(event));
        imgf_free(stale->pixels);
        delete stale;
    }, LV_EVENT_DELETE, image);
    return object;
}
