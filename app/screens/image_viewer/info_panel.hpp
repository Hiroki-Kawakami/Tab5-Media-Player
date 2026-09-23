/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <functional>
#include <string>
#include "lvgl.h"
#include "media/media_cache.hpp"

const char *image_kind_name(const MediaEntry &entry);

void image_info_panel_build(lv_obj_t *root, const std::string &name, const MediaEntry *entry,
                            const ImageSize *shown, bool rgb888,
                            std::function<void()> on_close);
