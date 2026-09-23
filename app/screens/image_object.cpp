/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "image_object.hpp"


namespace {

struct ImageHolder {
    lv_image_dsc_t dsc;
    std::shared_ptr<const ImagePixels> pixels;
};

lv_obj_t *create(lv_obj_t *parent, ImageHolder *image, const uint8_t *data, uint16_t width,
                 uint16_t height, uint32_t stride, bool rgb888) {
    image->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    image->dsc.header.cf = rgb888 ? LV_COLOR_FORMAT_RGB888 : LV_COLOR_FORMAT_RGB565;
    image->dsc.header.w = width;
    image->dsc.header.h = height;
    image->dsc.header.stride = stride;
    image->dsc.data = data;
    image->dsc.data_size = stride * height;

    lv_obj_t *object = lv_image_create(parent);
    lv_image_set_src(object, &image->dsc);
    lv_obj_center(object);
    lv_obj_add_event_cb(object, [](lv_event_t *event) {
        delete static_cast<ImageHolder *>(lv_event_get_user_data(event));
    }, LV_EVENT_DELETE, image);
    return object;
}

}

lv_obj_t *image_object_create(lv_obj_t *parent, std::shared_ptr<const ImagePixels> pixels) {
    if (!pixels || !pixels->data) return nullptr;

    auto image = new ImageHolder{};
    image->pixels = std::move(pixels);
    const ImagePixels &held = *image->pixels;
    return create(parent, image, held.data, held.width, held.height, held.stride, held.rgb888);
}

lv_obj_t *image_object_create(lv_obj_t *parent, const uint8_t *data, ImageSize size, bool rgb888) {
    if (!data || !size.valid()) return nullptr;
    const uint32_t stride = (uint32_t)size.width * (rgb888 ? 3 : 2);
    return create(parent, new ImageHolder{}, data, (uint16_t)size.width, (uint16_t)size.height,
                  stride, rgb888);
}
