/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "file_browser_screen.hpp"

#include <algorithm>
#include <dirent.h>
#include <strings.h>

FileBrowserScreen::FileBrowserScreen(std::string path, std::string title)
    : path_(std::move(path)), title_(std::move(title)) {}

void FileBrowserScreen::build() {
    createNavigation(title_.c_str(), LV_NAVIGATION_STYLE_LIST | LV_NAVIGATION_STYLE_BACK);
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(contents_, LV_OBJ_FLAG_SCROLLABLE);

    const bool opened = load_entries();
    if (!opened || entries_.empty()) {
        lv_obj_set_flex_align(contents_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        auto label = lv_label_create(contents_);
        lv_label_set_text(label, opened ? "No files" : "Cannot open directory");
        lv_obj_set_style_text_font(label, lv_widgets_body_font(), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x808080), 0);
        return;
    }

    lv_list_create(contents_, this);
}

bool FileBrowserScreen::load_entries() {
    DIR *dir = opendir(path_.c_str());
    if (!dir) return false;
    while (struct dirent *ent = readdir(dir)) {
        if (ent->d_name[0] == '.') continue;
        entries_.push_back({ent->d_name, ent->d_type == DT_DIR});
    }
    closedir(dir);

    std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
        if (a.directory != b.directory) return a.directory;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return true;
}

std::size_t FileBrowserScreen::rowCount() const {
    return entries_.size();
}

int32_t FileBrowserScreen::rowHeight() const {
    return 80;
}

lv_obj_t *FileBrowserScreen::createRow(lv_obj_t *parent) {
    lv_obj_t *row = lv_list_row_create(parent);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 24, 0);

    lv_obj_t *icon = lv_label_create(row);
    lv_obj_set_width(icon, 48);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(icon, lv_widgets_body_font(), 0);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_set_height(name, lv_font_get_line_height(lv_widgets_body_font()));
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(name, lv_widgets_body_font(), 0);

    lv_obj_t *arrow = lv_label_create(row);
    lv_obj_set_width(arrow, 48);
    lv_obj_set_style_text_align(arrow, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, lv_widgets_body_font(), 0);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x808080), 0);

    lv_obj_set_style_pad_hor(row, 24, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_t *separator = lv_hor_separator_create(row);
    lv_obj_add_flag(separator, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(separator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(separator, LV_ALIGN_BOTTOM_MID, 0, 0);
    return row;
}

void FileBrowserScreen::bindRow(lv_obj_t *row, std::size_t index) {
    const Entry &entry = entries_[index];
    lv_label_set_text(lv_obj_get_child(row, 0), entry.directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_FILE);
    lv_label_set_text(lv_obj_get_child(row, 1), entry.name.c_str());
    lv_obj_set_flag(lv_obj_get_child(row, 2), LV_OBJ_FLAG_HIDDEN, !entry.directory);
    lv_obj_set_flag(row, LV_OBJ_FLAG_CLICKABLE, entry.directory);
}

void FileBrowserScreen::didSelectRow(std::size_t index) {
    const Entry &entry = entries_[index];
    if (!entry.directory) return;
    screen_manager.push(std::make_shared<FileBrowserScreen>(path_ + "/" + entry.name, entry.name));
}
