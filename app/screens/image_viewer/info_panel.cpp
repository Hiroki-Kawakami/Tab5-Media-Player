/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "info_panel.hpp"
#include "screens/player_panel.hpp"
#include "screens/home/settings_widgets.hpp"
#include "widgets.hpp"

#include <cstdio>
#include <cstring>
#include <strings.h>

const char *image_kind_name(const MediaEntry &entry) {
    switch (entry.image_format) {
    case ImageFormat::Png: return "PNG";
    case ImageFormat::Jpeg: return entry.image_baseline ? "JPEG" : "progressive JPEG";
    default: return "";
    }
}

static const char *orientation_name(uint8_t value) {
    switch (value) {
    case 1: return "Normal";
    case 2: return "Mirrored";
    case 3: return "Rotate 180";
    case 4: return "Mirrored, 180";
    case 5: return "Mirrored, 90 CW";
    case 6: return "Rotate 90 CW";
    case 7: return "Mirrored, 270 CW";
    case 8: return "Rotate 270 CW";
    default: return nullptr;
    }
}

static void format_camera(char *out, std::size_t cap, const ImageExif &exif) {
    const std::size_t make = strlen(exif.make);
    if (!make) {
        snprintf(out, cap, "%s", exif.model);
    } else if (!exif.model[0]) {
        snprintf(out, cap, "%s", exif.make);
    } else if (strncasecmp(exif.model, exif.make, make) == 0) {
        snprintf(out, cap, "%s", exif.model);
    } else {
        snprintf(out, cap, "%s %s", exif.make, exif.model);
    }
}

/* "YYYY:MM:DD HH:MM:SS" is what EXIF stores. */
static void format_taken(char *out, std::size_t cap, const char *taken) {
    snprintf(out, cap, "%s", taken);
    if (strlen(out) >= 10 && out[4] == ':' && out[7] == ':') {
        out[4] = '-';
        out[7] = '-';
    }
}

static void format_shutter(char *out, std::size_t cap, uint32_t num, uint32_t den) {
    const double seconds = (double)num / den;
    if (seconds >= 1.0) {
        if (seconds == (double)(long)seconds) {
            snprintf(out, cap, "%.0f s", seconds);
        } else {
            snprintf(out, cap, "%.1f s", seconds);
        }
    } else if (num) {
        snprintf(out, cap, "1/%.0f s", (double)den / num);
    } else {
        snprintf(out, cap, "0 s");
    }
}

static void format_mm(char *out, std::size_t cap, float mm) {
    if (mm == (float)(long)mm) {
        snprintf(out, cap, "%.0f mm", (double)mm);
    } else {
        snprintf(out, cap, "%.1f mm", (double)mm);
    }
}

static void build_camera(lv_obj_t *contents, const ImageExif &exif) {
    if (!exif.make[0] && !exif.model[0] && !exif.lens[0] && !exif.software[0] && !exif.taken[0]) {
        return;
    }
    char text[96];

    auto section = lv_setting_section_create(contents, "Camera", &kPanelColors);
    lv_obj_update_layout(section);
    const int32_t width = lv_obj_get_content_width(section);
    if (exif.make[0] || exif.model[0]) {
        format_camera(text, sizeof(text), exif);
        panel_add_wide_row(section, "Camera", text, width);
    }
    if (exif.lens[0]) panel_add_wide_row(section, "Lens", exif.lens, width);
    if (exif.taken[0]) {
        format_taken(text, sizeof(text), exif.taken);
        panel_add_row(section, "Date Taken", text);
    }
    if (exif.software[0]) panel_add_wide_row(section, "Software", exif.software, width);
}

static void build_exposure(lv_obj_t *contents, const ImageExif &exif) {
    if (!exif.shutter_den && exif.aperture == 0 && !exif.iso && exif.focal_mm == 0 &&
        !exif.focal35_mm && !exif.has_bias && !exif.has_flash) {
        return;
    }
    char text[64];

    auto section = lv_setting_section_create(contents, "Exposure", &kPanelColors);
    if (exif.shutter_den) {
        format_shutter(text, sizeof(text), exif.shutter_num, exif.shutter_den);
        panel_add_row(section, "Shutter", text);
    }
    if (exif.aperture > 0) {
        snprintf(text, sizeof(text), "f/%.1f", (double)exif.aperture);
        panel_add_row(section, "Aperture", text);
    }
    if (exif.iso) {
        snprintf(text, sizeof(text), "%u", (unsigned)exif.iso);
        panel_add_row(section, "ISO", text);
    }
    if (exif.focal_mm > 0) {
        format_mm(text, sizeof(text), exif.focal_mm);
        panel_add_row(section, "Focal Length", text);
    }
    if (exif.focal35_mm) {
        snprintf(text, sizeof(text), "%u mm", (unsigned)exif.focal35_mm);
        panel_add_row(section, "35mm Equiv.", text);
    }
    if (exif.has_bias) {
        if (exif.exposure_bias == 0) {
            snprintf(text, sizeof(text), "0 EV");
        } else {
            snprintf(text, sizeof(text), "%+.1f EV", (double)exif.exposure_bias);
        }
        panel_add_row(section, "Exposure Bias", text);
    }
    if (exif.has_flash) {
        panel_add_row(section, "Flash", (exif.flash & 1) ? "Fired" : "Did not fire");
    }
}

void image_info_panel_build(lv_obj_t *root, const std::string &name, const MediaEntry *entry,
                            const ImageSize *shown, bool rgb888,
                            std::function<void()> on_close) {
    auto contents = player_panel_build(root, "Media Info", std::move(on_close));
    lv_obj_set_scroll_dir(contents, LV_DIR_VER);
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

    const ImageExif *exif = entry->image_exif.get();
    section = lv_setting_section_create(contents, "Image", &kPanelColors);
    if (entry->image_width && entry->image_height) {
        snprintf(text, sizeof(text), "%u x %u", (unsigned)entry->image_width,
                 (unsigned)entry->image_height);
        panel_add_row(section, "Resolution", text);
    }
    if (exif && orientation_name(exif->orientation)) {
        panel_add_row(section, "Orientation", orientation_name(exif->orientation));
    }
    if (shown) {
        snprintf(text, sizeof(text), "%u x %u", (unsigned)shown->width, (unsigned)shown->height);
        panel_add_row(section, "Shown At", text);
        panel_add_row(section, "Color", rgb888 ? "24-bit" : "16-bit");
    }
    if (!exif) return;
    build_camera(contents, *exif);
    build_exposure(contents, *exif);
}
