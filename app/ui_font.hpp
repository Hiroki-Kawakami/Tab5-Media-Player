/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once

// Builds the Montserrat -> NotoSansJP chains behind the widget font roles and
// installs them by overriding lv_widgets_font(). Must run after LVGL is up and
// before the first screen is created.
void ui_font_init();
