/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "player_panel.hpp"
#include "resources.h"
#include "widgets.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>

static constexpr int32_t kPadding = 24;
static constexpr int32_t kTitleIndent = 48;
static constexpr int32_t kCloseButton = 56;
static constexpr int32_t kHeaderHeight = kPadding + kCloseButton;

static constexpr int32_t kValueGap = 16;
static constexpr int32_t kValueLines = 3;

void panel_add_row(lv_obj_t *section, const char *label, const char *text,
                   const SettingColors *colors) {
    auto row = lv_setting_row_create(section, label);
    auto value = lv_setting_value_create(row, colors);
    lv_label_set_text(value, text);
}

static void set_wide_value(lv_obj_t *value, const char *label, const char *text, int32_t width) {
    const lv_font_t *font = lv_widgets_resolved_font(LV_WIDGETS_FONT_BODY);
    lv_label_set_text(value, text);

    const int32_t letter_space = lv_obj_get_style_text_letter_space(value, LV_PART_MAIN);
    const int32_t line_space = lv_obj_get_style_text_line_space(value, LV_PART_MAIN);
    const int32_t line_height = lv_font_get_line_height(font);
    lv_point_t title_size;
    lv_text_get_size(&title_size, label, font, letter_space, line_space, LV_COORD_MAX,
                     LV_TEXT_FLAG_NONE);
    lv_point_t size;
    lv_text_get_size(&size, text, font, letter_space, line_space,
                     width - title_size.x - kValueGap, LV_TEXT_FLAG_NONE);

    const int32_t limit = kValueLines * line_height + (kValueLines - 1) * line_space;
    if (size.y > limit) {
        lv_obj_set_height(value, limit);
        lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_DOTS);
    } else {
        lv_obj_set_height(value, LV_SIZE_CONTENT);
        lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_WRAP);
    }
}

void panel_add_wide_row(lv_obj_t *section, const char *label, const char *text, int32_t width,
                        const SettingColors *colors) {
    auto row = lv_container_create(section, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, kValueGap, 0);

    auto title = lv_label_create(row);
    lv_obj_set_style_text_font(title, lv_widgets_resolved_font(LV_WIDGETS_FONT_BODY), 0);
    lv_label_set_text(title, label);

    auto value = lv_setting_value_create(row, colors);
    lv_obj_set_flex_grow(value, 1);
    lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
    set_wide_value(value, label, text, width);
}

namespace {

struct ShownSections {
    std::vector<PanelSection> sections;
    std::vector<lv_obj_t *> objects;
};

}

static bool same_layout(const std::vector<PanelSection> &a, const std::vector<PanelSection> &b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); i++) {
        if (strcmp(a[i].title ? a[i].title : "", b[i].title ? b[i].title : "") != 0) return false;
        if (a[i].rows.size() != b[i].rows.size()) return false;
        for (std::size_t j = 0; j < a[i].rows.size(); j++) {
            if (a[i].rows[j].label != b[i].rows[j].label) return false;
            if (a[i].rows[j].wide != b[i].rows[j].wide) return false;
        }
    }
    return true;
}

void panel_sections_show(lv_obj_t *contents, const std::vector<PanelSection> &sections,
                         const SettingColors *colors) {
    auto *shown = static_cast<ShownSections *>(lv_obj_get_user_data(contents));
    if (!shown) {
        shown = new ShownSections;
        lv_obj_set_user_data(contents, shown);
        lv_obj_add_event_cb(contents, [](lv_event_t *event) {
            delete static_cast<ShownSections *>(lv_event_get_user_data(event));
        }, LV_EVENT_DELETE, shown);
    } else if (same_layout(shown->sections, sections)) {
        for (std::size_t i = 0; i < sections.size(); i++) {
            lv_obj_t *section = shown->objects[i];
            for (std::size_t j = 0; j < sections[i].rows.size(); j++) {
                const PanelRow &row = sections[i].rows[j];
                if (row.value == shown->sections[i].rows[j].value) continue;
                lv_obj_t *value = lv_obj_get_child(lv_obj_get_child(section, (int32_t)j), 1);
                if (row.wide) {
                    set_wide_value(value, row.label.c_str(), row.value.c_str(),
                                   lv_obj_get_content_width(section));
                } else {
                    lv_label_set_text(value, row.value.c_str());
                }
            }
        }
        shown->sections = sections;
        return;
    }

    const int32_t scroll = lv_obj_get_scroll_y(contents);
    lv_obj_clean(contents);
    shown->objects.clear();
    for (const PanelSection &entry : sections) {
        lv_obj_t *section = lv_setting_section_create(contents, entry.title, colors);
        lv_obj_update_layout(section);
        const int32_t width = lv_obj_get_content_width(section);
        for (const PanelRow &row : entry.rows) {
            if (row.wide) {
                panel_add_wide_row(section, row.label.c_str(), row.value.c_str(), width, colors);
            } else {
                panel_add_row(section, row.label.c_str(), row.value.c_str(), colors);
            }
        }
        shown->objects.push_back(section);
    }
    shown->sections = sections;
    if (scroll <= 0) return;
    lv_obj_update_layout(contents);
    const int32_t limit =
        lv_obj_get_scroll_y(contents) + std::max<int32_t>(lv_obj_get_scroll_bottom(contents), 0);
    lv_obj_scroll_to_y(contents, std::min(scroll, limit), LV_ANIM_OFF);
}

static void format_size(char *out, size_t size, int64_t bytes) {
    if (bytes >= 1024 * 1024 * 1024) {
        snprintf(out, size, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024 * 1024) {
        snprintf(out, size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, size, "%.1f KB", (double)bytes / 1024.0);
    }
}

void panel_format_size(char *out, std::size_t size, int64_t bytes) {
    if (bytes >= 1024 * 1024 * 1024) {
        snprintf(out, size, "%.2f GB", (double)bytes / (1024.0 * 1024.0 * 1024.0));
    } else if (bytes >= 1024 * 1024) {
        snprintf(out, size, "%.1f MB", (double)bytes / (1024.0 * 1024.0));
    } else {
        snprintf(out, size, "%.1f KB", (double)bytes / 1024.0);
    }
}

static void build_header(lv_obj_t *root, const char *title, std::function<void()> on_close) {
    auto header = lv_container_create(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(header, lv_pct(100), kHeaderHeight);
    lv_obj_set_style_pad_left(header, kTitleIndent, 0);
    lv_obj_set_style_pad_right(header, kPadding, 0);
    lv_obj_set_style_pad_top(header, kPadding, 0);
    lv_obj_set_style_bg_color(header, lv_color_hex(kPanelColors.page_bg), 0);
    lv_obj_set_style_bg_opa(header, LV_OPA_COVER, 0);

    auto label = lv_label_create(header);
    lv_obj_set_flex_grow(label, 1);
    lv_obj_set_font_role(label, LV_WIDGETS_FONT_TITLE);
    lv_label_set_text(label, title);

    auto close = lv_button_create(header, LV_BUTTON_STYLE_PLAIN);
    lv_obj_set_size(close, kCloseButton, kCloseButton);
    lv_obj_set_style_pad_all(close, 0, 0);
    lv_obj_set_style_radius(close, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(close, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(close, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(close, lv_color_white(), 0);
    lv_button_set_text(close, TABLER_X, &icon_36);
    lv_obj_add_event_fn(close, LV_EVENT_CLICKED, [on_close](lv_event_t *) { on_close(); });
}

lv_obj_t *player_panel_build(lv_obj_t *root, const char *title, std::function<void()> on_close) {
    lv_obj_set_style_bg_color(root, lv_color_hex(kPanelColors.page_bg), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(root, lv_color_white(), 0);
    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scrollable(root, false);

    build_header(root, title, std::move(on_close));

    auto contents = lv_spacer_create(root, lv_pct(100), LV_SIZE_CONTENT, 1);
    lv_obj_set_flex_flow(contents, LV_FLEX_FLOW_COLUMN);
    lv_setting_page_style(contents, &kPanelColors);
    return contents;
}
