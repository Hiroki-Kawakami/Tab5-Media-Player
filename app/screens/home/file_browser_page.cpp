/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "file_browser_page.hpp"
#include "screens/home_screen.hpp"
#include "media/demuxer.hpp"
#include "media_player.hpp"
#include "screen_manager.hpp"
#include "screens/audio_player_screen.hpp"
#include "screens/video_player_screen.hpp"

#include <algorithm>
#include <dirent.h>
#include <strings.h>

FileBrowserPage::FileBrowserPage(std::string path, std::string title)
    : path_(std::move(path)), title_(std::move(title)) {}

void FileBrowserPage::build(lv_obj_t *contents) {
    lv_obj_set_style_bg_color(contents, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(contents, LV_OPA_COVER, 0);
    lv_obj_remove_flag(contents, LV_OBJ_FLAG_SCROLLABLE);
    list_ = nullptr;

    if (!loaded_) {
        opened_ = load_entries();
        loaded_ = true;
    }
    if (!opened_ || entries_.empty()) {
        lv_obj_set_flex_align(contents, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        auto label = lv_label_create(contents);
        lv_label_set_text(label, opened_ ? "No files" : "Cannot open directory");
        lv_obj_set_style_text_font(label, lv_widgets_body_font(), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(0x808080), 0);
        return;
    }

    list_ = lv_list_create(contents, this);
    if (scroll_y_ > 0) lv_obj_scroll_to_y(list_, scroll_y_, LV_ANIM_OFF);
}

void FileBrowserPage::save_state() {
    if (!list_) return;
    scroll_y_ = lv_obj_get_scroll_y(list_);
    list_ = nullptr;
}

bool FileBrowserPage::is_under(const std::string &mount_point) const {
    return path_is_under(path_, mount_point);
}

bool FileBrowserPage::load_entries() {
    DIR *dir = opendir(path_.c_str());
    if (!dir) return false;
    while (struct dirent *ent = readdir(dir)) {
        if (ent->d_name[0] == '.') continue;
        const bool directory = ent->d_type == DT_DIR;
        const MediaKind kind = directory ? MediaKind::None : demuxer_media_kind(ent->d_name);
        entries_.push_back({ent->d_name, directory, kind});
    }
    closedir(dir);

    std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
        if (a.directory != b.directory) return a.directory;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return true;
}

std::size_t FileBrowserPage::rowCount() const {
    return entries_.size();
}

int32_t FileBrowserPage::rowHeight() const {
    return 80;
}

lv_obj_t *FileBrowserPage::createRow(lv_obj_t *parent) {
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

void FileBrowserPage::bindRow(lv_obj_t *row, std::size_t index) {
    const Entry &entry = entries_[index];
    const char *icon = entry.directory                ? LV_SYMBOL_DIRECTORY
                     : entry.kind == MediaKind::Video ? LV_SYMBOL_VIDEO
                     : entry.kind == MediaKind::Audio ? LV_SYMBOL_AUDIO
                                                      : LV_SYMBOL_FILE;
    lv_label_set_text(lv_obj_get_child(row, 0), icon);
    lv_label_set_text(lv_obj_get_child(row, 1), entry.name.c_str());
    lv_obj_set_flag(lv_obj_get_child(row, 2), LV_OBJ_FLAG_HIDDEN, !entry.directory);
    lv_obj_set_flag(row, LV_OBJ_FLAG_CLICKABLE,
                    entry.directory || entry.kind != MediaKind::None);
}

std::shared_ptr<Playlist> FileBrowserPage::make_playlist(std::size_t index) const {
    const MediaKind kind = entries_[index].kind;
    std::vector<PlaylistItem> items;
    std::size_t current = 0;
    for (std::size_t i = 0; i < entries_.size(); i++) {
        const Entry &entry = entries_[i];
        if (entry.directory || entry.kind != kind) continue;
        if (i == index) current = items.size();
        items.push_back({ entry.name, path_ + "/" + entry.name });
    }
    return std::make_shared<Playlist>(std::move(items), current);
}

void FileBrowserPage::didSelectRow(std::size_t index) {
    const Entry &entry = entries_[index];
    if (entry.directory) {
        home_->push(std::make_shared<FileBrowserPage>(path_ + "/" + entry.name, entry.name));
    } else if (entry.kind == MediaKind::Audio) {
        screen_manager.push(std::make_shared<AudioPlayerScreen>(make_playlist(index)));
    } else if (entry.kind == MediaKind::Video) {
        screen_manager.push(std::make_shared<VideoPlayerScreen>(make_playlist(index)));
    }
}
