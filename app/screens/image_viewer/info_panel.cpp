/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "info_panel.hpp"
#include "screens/player_panel.hpp"
#include "screens/home/settings_widgets.hpp"
#include "widgets.hpp"

#include <cstdio>

const char *image_kind_name(const MediaEntry &entry) {
    switch (entry.image_format) {
    case ImageFormat::Png: return "PNG";
    case ImageFormat::Jpeg: return entry.image_baseline ? "JPEG" : "progressive JPEG";
    default: return "";
    }
}

void image_info_panel_build(lv_obj_t *root, const std::string &name, const MediaEntry *entry,
                            const ImagePixels *shown, std::function<void()> on_close) {
    auto contents = player_panel_build(root, "Media Info", std::move(on_close));
    char text[64];

    auto section = lv_setting_section_create(contents, "General", &kPanelColors);
    lv_obj_update_layout(section);
    panel_add_wide_row(section, "File", name.c_str(), lv_obj_get_content_width(section));
    if (!entry) return;

    const char *kind = image_kind_name(*entry);
    if (kind[0]) panel_add_row(section, "Format", kind);
    if (entry->file_bytes > 0) {
        panel_format_size(text, sizeof(text), entry->file_bytes);
        panel_add_row(section, "Size", text);
    }

    section = lv_setting_section_create(contents, "Image", &kPanelColors);
    if (entry->image_width && entry->image_height) {
        snprintf(text, sizeof(text), "%u x %u", (unsigned)entry->image_width,
                 (unsigned)entry->image_height);
        panel_add_row(section, "Resolution", text);
    }
    if (shown) {
        snprintf(text, sizeof(text), "%u x %u", (unsigned)shown->width, (unsigned)shown->height);
        panel_add_row(section, "Shown At", text);
        panel_add_row(section, "Color", shown->rgb888 ? "24-bit" : "16-bit");
    }
}
