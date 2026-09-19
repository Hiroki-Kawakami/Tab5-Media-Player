/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "lvgl.h"

lv_obj_t *lv_grouped_section_create(lv_obj_t *parent, const char *title = nullptr);
lv_obj_t *lv_grouped_row_create(lv_obj_t *section, const char *icon, const char *label);
void lv_grouped_row_set_arrow_visible(lv_obj_t *row, bool visible);
