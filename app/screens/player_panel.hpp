/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "lvgl.h"
#include "screens/home/settings_widgets.hpp"

inline constexpr SettingColors kPanelColors = {
    .page_bg    = 0x101010,
    .section_bg = 0x1e1e1e,
    .value      = 0x9e9e9e,
    .separator  = 0x3a3a3a,
    .track      = 0x404040,
    .accent     = 0x2196f3,
};

/* Dark panel over the video: title row with a close button, and the contents
 * container it returns, which the caller fills with sections. */
lv_obj_t *player_panel_build(lv_obj_t *root, const char *title, std::function<void()> on_close);

void panel_add_row(lv_obj_t *section, const char *label, const char *text,
                   const SettingColors *colors = &kPanelColors);
/* A value that may need more than one line: `width` is the section's content
   width, which decides where the text is clipped to three lines. */
void panel_add_wide_row(lv_obj_t *section, const char *label, const char *text, int32_t width,
                        const SettingColors *colors = &kPanelColors);
void panel_format_size(char *out, std::size_t size, int64_t bytes);

struct PanelRow {
    std::string label;
    std::string value;
    bool wide = false;
};

struct PanelSection {
    const char *title;
    std::vector<PanelRow> rows;
};

/* Fills `contents` with the sections. Shown again on the same contents with the
   same titles and labels, only the values that differ change, and the scroll
   position stays where it was. */
void panel_sections_show(lv_obj_t *contents, const std::vector<PanelSection> &sections,
                         const SettingColors *colors = &kPanelColors);
