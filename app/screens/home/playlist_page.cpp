/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "playlist_page.hpp"
#include "media/media_cache.hpp"
#include "media_player.hpp"
#include "resources.h"
#include "screen_manager.hpp"
#include "screens/audio_player_screen.hpp"
#include "screens/image_object.hpp"
#include "screens/image_viewer_screen.hpp"
#include "screens/player_layout.hpp"
#include "screens/video_player_screen.hpp"

#include <algorithm>
#include <memory>

static constexpr uint32_t kResolveTimeoutMs = 700;
static constexpr std::size_t kLookahead = 2;
static constexpr int32_t kPad = 24;
static constexpr int32_t kTrackRowHeight = 72;
static constexpr int32_t kTrackThumbSide = 56;
static constexpr ImageSize kTrackThumbBox = { kTrackThumbSide, kTrackThumbSide };
static constexpr int32_t kWideRowHeight = 132;
static constexpr ImageSize kWideThumbBox = { 192, 108 };
static constexpr uint32_t kWideThumbColor = 0xeeeeee;

static PlaylistPage *s_visible;

static int32_t body_line_height() {
    return lv_font_get_line_height(lv_widgets_resolved_font(LV_WIDGETS_FONT_BODY));
}

static std::string format_names(const std::string &artist, const std::string &album) {
    if (artist.empty()) return album;
    if (album.empty()) return artist;
    return artist + " · " + album;
}

static lv_obj_t *thumb_frame_create(lv_obj_t *row, ImageSize box, uint32_t color, const char *icon,
                                    const lv_font_t *icon_font) {
    lv_obj_t *frame = lv_container_create(row, lv_color_hex(color));
    lv_obj_set_size(frame, box.width, box.height);
    lv_obj_set_style_radius(frame, 6, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_clickable(frame, false);
    lv_obj_set_scrollable(frame, false);
    lv_obj_t *label = lv_label_create(frame);
    lv_label_set_text(label, icon);
    lv_obj_set_style_text_font(label, icon_font, 0);
    lv_obj_center(label);
    return frame;
}

static void bind_thumbnail(lv_obj_t *frame, const std::string &path, ImageSize box) {
    std::shared_ptr<const ImagePixels> pixels = media_cache_thumbnail(path, box);
    lv_obj_t *image = lv_obj_get_child_count(frame) > 1 ? lv_obj_get_child(frame, 1) : nullptr;
    if (image && lv_obj_get_user_data(image) != pixels.get()) {
        lv_obj_delete(image);
        image = nullptr;
    }
    if (!image && pixels) {
        const ImagePixels *shown = pixels.get();
        image = image_object_create(frame, std::move(pixels));
        if (image) {
            lv_obj_set_user_data(image, const_cast<ImagePixels *>(shown));
            lv_obj_set_size(image, box.width, box.height);
            lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
        }
    }
    lv_obj_set_hidden(lv_obj_get_child(frame, 0), image != nullptr);
}

PlaylistPage::PlaylistPage(const std::string &path) : path_(path) {
    opened_ = playlist_file_read(path, &file_);
}

PlaylistPage::~PlaylistPage() {
    release_requests();
}

void PlaylistPage::build(lv_obj_t *contents) {
    lv_obj_set_scrollable(contents, false);
    lv_obj_set_style_bg_color(contents, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(contents, LV_OPA_COVER, 0);
    if (!opened_ || file_.items.empty()) {
        lv_obj_set_flex_align(contents, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_t *label = lv_label_create(contents);
        lv_label_set_text(label, opened_ ? "No files" : "Cannot open playlist");
        lv_obj_set_font_role(label, LV_WIDGETS_FONT_BODY);
        lv_obj_set_style_text_color(label, lv_color_hex(0x808080), 0);
        return;
    }
    list_ = lv_list_create(contents, this);
    lv_obj_add_event_fn(list_, LV_EVENT_SCROLL, [this](lv_event_t *) { request_rows(); });
    lv_obj_update_layout(contents);
    if (scroll_y_ > 0) lv_obj_scroll_to_y(list_, scroll_y_, LV_ANIM_OFF);
    observe();
}

void PlaylistPage::save_state() {
    release_requests();
    if (list_) scroll_y_ = lv_obj_get_scroll_y(list_);
    list_ = nullptr;
}

void PlaylistPage::on_appear() {
    observe();
}

void PlaylistPage::on_disappear() {
    release_requests();
}

bool PlaylistPage::is_under(const std::string &mount_point) const {
    return path_is_under(path_, mount_point);
}

ImageSize PlaylistPage::thumb_box() const {
    return tracks() ? kTrackThumbBox : kWideThumbBox;
}

void PlaylistPage::observe() {
    if (!list_) return;
    s_visible = this;
    requested_first_ = SIZE_MAX;
    if (!token_) token_ = media_cache_token();
    media_cache_observe(token_, meta_ready);
    request_rows();
}

void PlaylistPage::release_requests() {
    if (s_visible == this) s_visible = nullptr;
    visible_paths_.clear();
    if (!token_) return;
    media_cache_unobserve(token_);
    media_cache_cancel(token_);
}

bool PlaylistPage::keep_visible(const char *path, void *ctx) {
    for (const std::string &kept : static_cast<PlaylistPage *>(ctx)->visible_paths_) {
        if (kept == path) return true;
    }
    return false;
}

void PlaylistPage::request_rows() {
    if (!list_) return;
    std::size_t first = lv_list_first_visible_row(list_);
    if (first == SIZE_MAX) first = 0;
    if (first == requested_first_) return;
    requested_first_ = first;

    const std::size_t rows = (std::size_t)(lv_obj_get_height(list_) / rowHeight()) + 1;
    const std::size_t start = first > kLookahead ? first - kLookahead : 0;
    const std::size_t end = std::min(file_.items.size(), first + rows + kLookahead);
    visible_paths_.clear();
    for (std::size_t i = start; i < end; i++) visible_paths_.push_back(file_.items[i].path);
    media_cache_retain(token_, keep_visible, this);

    if (file_.kind != MediaKind::Image) {
        for (const std::string &row : visible_paths_) {
            if (media_cache_lookup(row)) continue;
            media_cache_request(row, MetaWantInfo, {}, MetaPriority::Visible, token_);
        }
    }
    for (const std::string &row : visible_paths_) {
        const std::shared_ptr<const MediaEntry> meta = media_cache_lookup(row);
        if (meta && meta->cover_scanned && !meta->has_cover) continue;
        if (media_cache_thumbnail(row, thumb_box())) continue;
        media_cache_request(row, MetaWantInfo | MetaWantThumbnail, thumb_box(),
                            MetaPriority::Visible, token_);
    }
}

void PlaylistPage::meta_ready(const std::string &path) {
    if (s_visible) s_visible->rebind_path(path);
}

void PlaylistPage::rebind_path(const std::string &path) {
    if (!list_) return;
    for (std::size_t i = 0; i < file_.items.size(); i++) {
        if (file_.items[i].path != path) continue;
        if (lv_obj_t *row = lv_list_row_for_index(list_, i)) bindRow(row, i);
    }
}

std::size_t PlaylistPage::rowCount() const {
    return file_.items.size();
}

int32_t PlaylistPage::rowHeight() const {
    return tracks() ? kTrackRowHeight : kWideRowHeight;
}

lv_obj_t *PlaylistPage::createRow(lv_obj_t *parent) {
    lv_obj_t *row = lv_list_row_create(parent);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(row, kPad, 0);
    lv_obj_set_style_border_width(row, 0, 0);

    if (tracks()) {
        lv_obj_set_style_pad_column(row, 16, 0);
        lv_obj_t *frame =
            thumb_frame_create(row, kTrackThumbBox, kPlayerArtworkColor, TABLER_MUSIC, &icon_28);
        lv_obj_set_style_text_color(lv_obj_get_child(frame, 0), lv_color_hex(kPlayerArtworkIconColor),
                                    0);
        lv_obj_t *text = lv_container_create(row);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_height(text, LV_SIZE_CONTENT);
        lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_clickable(text, false);
        for (int i = 0; i < 2; i++) {
            lv_obj_t *label = lv_label_create(text);
            lv_obj_set_size(label, lv_pct(100), body_line_height());
            lv_obj_set_font_role(label, LV_WIDGETS_FONT_BODY);
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
            if (i) lv_obj_set_style_text_color(label, lv_color_hex(kPlayerSubtitleColor), 0);
        }
    } else {
        lv_obj_set_style_pad_column(row, 24, 0);
        thumb_frame_create(row, kWideThumbBox, kWideThumbColor,
                           file_.kind == MediaKind::Image ? TABLER_PHOTO : TABLER_MOVIE, &icon_48);
        lv_obj_t *name = lv_label_create(row);
        lv_obj_set_flex_grow(name, 1);
        lv_obj_set_height(name, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(name, 2 * body_line_height(), 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_font_role(name, LV_WIDGETS_FONT_BODY);
    }

    lv_obj_t *separator = lv_hor_separator_create(row);
    lv_obj_set_ignore_layout(separator, true);
    lv_obj_set_clickable(separator, false);
    lv_obj_align(separator, LV_ALIGN_BOTTOM_MID, 0, 0);
    return row;
}

void PlaylistPage::bindRow(lv_obj_t *row, std::size_t index) {
    const PlaylistItem &item = file_.items[index];
    const std::shared_ptr<const MediaEntry> meta = media_cache_lookup(item.path);
    const char *title = meta && meta->title[0] ? meta->title : item.name.c_str();
    lv_obj_set_clickable(row, true);
    if (tracks()) {
        const std::string names = meta ? format_names(meta->artist, meta->album) : std::string();
        lv_obj_t *text = lv_obj_get_child(row, 1);
        lv_obj_t *subtitle = lv_obj_get_child(text, 1);
        lv_label_set_text(lv_obj_get_child(text, 0), title);
        lv_label_set_text(subtitle, names.c_str());
        lv_obj_set_hidden(subtitle, names.empty());
    } else {
        lv_label_set_text(lv_obj_get_child(row, 1), title);
    }
    bind_thumbnail(lv_obj_get_child(row, 0), item.path, thumb_box());
}

void PlaylistPage::didSelectRow(std::size_t index) {
    auto playlist = std::make_shared<Playlist>(file_.items, index);
    switch (file_.kind) {
    case MediaKind::Audio:
        media_cache_resolve(file_.items[index].path, MetaWantInfo, {}, kResolveTimeoutMs);
        screen_manager.push(std::make_shared<AudioPlayerScreen>(std::move(playlist)));
        break;
    case MediaKind::Video:
        screen_manager.push(std::make_shared<VideoPlayerScreen>(std::move(playlist)));
        break;
    case MediaKind::Image:
        screen_manager.push(std::make_shared<ImageViewerScreen>(std::move(playlist)));
        break;
    default:
        break;
    }
}
