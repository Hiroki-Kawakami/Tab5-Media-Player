/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "file_browser_page.hpp"
#include "screens/home_screen.hpp"
#include "media/demuxer.hpp"
#include "media/media_cache.hpp"
#include "media_player.hpp"
#include "screen_manager.hpp"
#include "screens/audio_player_screen.hpp"
#include "screens/cover_art.hpp"
#include "screens/video_player_screen.hpp"

#include <algorithm>
#include <dirent.h>
#include <strings.h>

static constexpr int32_t kRowHeight = 80;
static constexpr int32_t kThumbSide = 56;
static constexpr std::size_t kLookahead = 2;
static constexpr std::size_t kPrefetchLimit = 24;
static constexpr uint32_t kResolveTimeoutMs = 700;

static FileBrowserPage *s_visible;

FileBrowserPage::FileBrowserPage(std::string path, std::string title)
    : path_(std::move(path)), title_(std::move(title)) {}

FileBrowserPage::~FileBrowserPage() {
    release_requests();
}

void FileBrowserPage::release_requests() {
    if (s_visible == this) s_visible = nullptr;
    if (!token_) return;
    media_cache_unobserve(token_);
    media_cache_cancel(token_);
    media_cache_cancel(idle_token_);
}

std::string FileBrowserPage::entry_path(std::size_t index) const {
    return path_ + "/" + entries_[index].name.c_str();
}

void FileBrowserPage::request_visible() {
    if (!list_ || entries_.empty()) return;

    std::size_t first = lv_list_first_visible_row(list_);
    if (first == SIZE_MAX) first = 0;
    if (first == requested_first_) return;
    requested_first_ = first;
    const int32_t height = lv_obj_get_height(list_);
    const std::size_t rows = height > 0 ? (std::size_t)(height / kRowHeight) + 1 : 1;
    const std::size_t start = first > kLookahead ? first - kLookahead : 0;
    const std::size_t end = std::min(entries_.size(), first + rows + kLookahead);

    media_cache_cancel(token_);
    for (std::size_t i = start; i < end; i++) {
        if (entries_[i].directory || entries_[i].kind == MediaKind::None) continue;
        media_cache_request(entry_path(i), MetaWantInfo | MetaWantImage, kThumbSide,
                            MetaPriority::Visible, token_);
    }
    prefetch_rest();
}

void FileBrowserPage::prefetch_rest() {
    if (prefetched_) return;
    prefetched_ = true;
    std::size_t queued = 0;
    for (std::size_t i = 0; i < entries_.size() && queued < kPrefetchLimit; i++) {
        if (entries_[i].directory || entries_[i].kind != MediaKind::Audio) continue;
        media_cache_request(entry_path(i), MetaWantInfo | MetaWantImage, kThumbSide,
                            MetaPriority::Idle, idle_token_);
        queued++;
    }
}

void FileBrowserPage::meta_ready(const std::string &path) {
    FileBrowserPage *page = s_visible;
    if (!page || !page->list_) return;
    if (path.compare(0, page->path_.size(), page->path_) != 0) return;

    for (std::size_t i = 0; i < page->entries_.size(); i++) {
        if (page->entry_path(i) != path) continue;
        if (lv_obj_t *row = lv_list_row_for_index(page->list_, i)) page->bindRow(row, i);
        return;
    }
}

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

    s_visible = this;
    requested_first_ = SIZE_MAX;
    if (!token_) {
        token_ = media_cache_token();
        idle_token_ = media_cache_token();
    }
    media_cache_observe(token_, meta_ready);
    lv_obj_add_event_fn(list_, LV_EVENT_SCROLL, [this](lv_event_t *) { request_visible(); });
    lv_obj_update_layout(contents);
    request_visible();
}

void FileBrowserPage::on_appear() {
    if (!list_) return;
    s_visible = this;
    requested_first_ = SIZE_MAX;
    prefetched_ = false;
    media_cache_observe(token_, meta_ready);
    request_visible();
}

void FileBrowserPage::save_state() {
    release_requests();
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
        entries_.push_back({ PsramString(ent->d_name), directory, kind });
    }
    closedir(dir);

    std::sort(entries_.begin(), entries_.end(), [](const Entry &a, const Entry &b) {
        if (a.directory != b.directory) return a.directory;
        return strcasecmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    entries_.shrink_to_fit();
    return true;
}

std::size_t FileBrowserPage::rowCount() const {
    return entries_.size();
}

int32_t FileBrowserPage::rowHeight() const {
    return kRowHeight;
}

lv_obj_t *FileBrowserPage::createRow(lv_obj_t *parent) {
    lv_obj_t *row = lv_list_row_create(parent);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 24, 0);

    lv_obj_t *icon = lv_label_create(row);
    lv_obj_set_width(icon, kThumbSide);
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
    lv_obj_t *first = lv_obj_get_child(row, 0);
    if (lv_obj_check_type(first, &lv_image_class)) lv_obj_delete(first);
    lv_obj_t *label = lv_obj_get_child(row, 0);
    lv_label_set_text(label, icon);
    lv_label_set_text(lv_obj_get_child(row, 1), entry.name.c_str());
    lv_obj_set_flag(lv_obj_get_child(row, 2), LV_OBJ_FLAG_HIDDEN, !entry.directory);
    lv_obj_set_flag(row, LV_OBJ_FLAG_CLICKABLE,
                    entry.directory || entry.kind != MediaKind::None);

    std::shared_ptr<const CoverPixels> pixels;
    if (!entry.directory && entry.kind != MediaKind::None) {
        pixels = media_cache_image(entry_path(index), kThumbSide);
    }
    lv_obj_set_flag(label, LV_OBJ_FLAG_HIDDEN, pixels != nullptr);
    /* Inserted first so flex puts it where the icon was; everything indexed
       above is therefore read before this point. */
    if (lv_obj_t *image = pixels ? cover_art_create(row, std::move(pixels)) : nullptr) {
        lv_obj_set_size(image, kThumbSide, kThumbSide);
        lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
        lv_obj_set_style_radius(image, 6, 0);
        lv_obj_set_style_clip_corner(image, true, 0);
        lv_obj_move_to_index(image, 0);
    }
}

std::shared_ptr<Playlist> FileBrowserPage::make_playlist(std::size_t index) const {
    const MediaKind kind = entries_[index].kind;
    std::vector<PlaylistItem> items;
    std::size_t current = 0;
    for (std::size_t i = 0; i < entries_.size(); i++) {
        const Entry &entry = entries_[i];
        if (entry.directory || entry.kind != kind) continue;
        if (i == index) current = items.size();
        items.push_back({ entry.name.c_str(), entry_path(i) });
    }
    return std::make_shared<Playlist>(std::move(items), current);
}

void FileBrowserPage::didSelectRow(std::size_t index) {
    const Entry &entry = entries_[index];
    if (entry.directory) {
        home_->push(std::make_shared<FileBrowserPage>(entry_path(index), entry.name.c_str()));
    } else if (entry.kind == MediaKind::Audio) {
        media_cache_resolve(entry_path(index), MetaWantInfo, 0, kResolveTimeoutMs);
        screen_manager.push(std::make_shared<AudioPlayerScreen>(make_playlist(index)));
    } else if (entry.kind == MediaKind::Video) {
        screen_manager.push(std::make_shared<VideoPlayerScreen>(make_playlist(index)));
    }
}
