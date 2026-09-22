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

}

lv_obj_t *image_object_create(lv_obj_t *parent, std::shared_ptr<const ImagePixels> pixels) {
    if (!pixels || !pixels->data) return nullptr;

    auto image = new ImageHolder{};
    image->pixels = std::move(pixels);
    image->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    image->dsc.header.cf = image->pixels->rgb888 ? LV_COLOR_FORMAT_RGB888 : LV_COLOR_FORMAT_RGB565;
    image->dsc.header.w = image->pixels->width;
    image->dsc.header.h = image->pixels->height;
    image->dsc.header.stride = image->pixels->stride;
    image->dsc.data = image->pixels->data;
    image->dsc.data_size = (uint32_t)image->pixels->bytes;

    lv_obj_t *object = lv_image_create(parent);
    lv_image_set_src(object, &image->dsc);
    lv_obj_center(object);
    lv_obj_add_event_cb(object, [](lv_event_t *event) {
        delete static_cast<ImageHolder *>(lv_event_get_user_data(event));
    }, LV_EVENT_DELETE, image);
    return object;
}
