/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <functional>
#include <initializer_list>
#include "lvgl.h"

void lv_setting_page_style(lv_obj_t *contents);
lv_obj_t *lv_setting_section_create(lv_obj_t *contents);
lv_obj_t *lv_setting_row_create(lv_obj_t *section, const char *label);
lv_obj_t *lv_setting_value_create(lv_obj_t *row);
lv_obj_t *lv_setting_slider_create(lv_obj_t *section, int32_t min, int32_t max, int32_t value);
lv_obj_t *lv_setting_segmented_create(lv_obj_t *row, std::initializer_list<const char *> labels,
                                      int active,
                                      std::function<void(lv_obj_t *, int)> on_select);
void lv_setting_segmented_set_active(lv_obj_t *segmented, int active);
lv_obj_t *lv_setting_switch_create(lv_obj_t *row, bool checked,
                                   std::function<void(lv_obj_t *, bool)> on_change);
