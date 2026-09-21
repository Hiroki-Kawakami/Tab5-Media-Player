/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/media_types.hpp"
#include "widgets.hpp"

/* Decodes the artwork into an image centred in `parent`, scaled to fit a
 * `side` px square. Returns nullptr when there is nothing to show; the pixels
 * belong to the returned object. */
lv_obj_t *cover_art_create(lv_obj_t *parent, const CoverArt &cover, int32_t side);
