/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/artwork_codec.hpp"
#include "widgets.hpp"

#include <memory>

lv_obj_t *cover_art_create(lv_obj_t *parent, std::shared_ptr<const CoverPixels> pixels);
