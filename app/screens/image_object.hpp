/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/artwork_codec.hpp"
#include "widgets.hpp"

#include <memory>

lv_obj_t *image_object_create(lv_obj_t *parent, std::shared_ptr<const ImagePixels> pixels);
