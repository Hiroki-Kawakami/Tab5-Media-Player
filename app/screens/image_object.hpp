/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/image_codec.hpp"
#include "media/image_pixels.hpp"
#include "widgets.hpp"

#include <memory>

lv_obj_t *image_object_create(lv_obj_t *parent, std::shared_ptr<const ImagePixels> pixels);
/* `data` is borrowed: it has to outlive the object. */
lv_obj_t *image_object_create(lv_obj_t *parent, const uint8_t *data, ImageSize size, bool rgb888);
