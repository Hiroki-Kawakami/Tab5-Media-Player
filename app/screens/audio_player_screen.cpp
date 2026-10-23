/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "audio_player_screen.hpp"
#include "media_player.hpp"
#include "screens/image_object.hpp"
#include "screens/media_controls.hpp"
#include "screens/video_player/info_panel.hpp"
#include "screens/video_player/settings_panel.hpp"
#include "settings.hpp"
#include "resources.h"
#include "bsp.h"
#include "esp_timer.h"

#include <algorithm>
#include <cstdio>

static constexpr uint32_t kRefreshPeriodMs = 500;
static constexpr uint32_t kResolveTimeoutMs = 700;
static constexpr int64_t kPrefetchDelayUs = 2000000;
static constexpr int64_t kPrevTrackUs = 3000000;
static constexpr int32_t kSeekRange = 1000;
static constexpr uint32_t kMarqueePauseMs = 2000;
static constexpr int32_t kPad = 24;
static constexpr int32_t kGap = 24;
static constexpr int32_t kTimeWidth = 100;
static constexpr int32_t kTitleGap = 12;
static constexpr int32_t kPortraitTitleBottomPad = 8;
static constexpr int32_t kLandscapeTitleBottomPad = 20;
static constexpr int32_t kSeekRowHeight = 72;
static constexpr int32_t kIconButton = 72;
static constexpr int32_t kHeaderHeight = 120;
static constexpr int32_t kSegmentHeight = 64;
static constexpr int32_t kSegmentGap = 16;
static constexpr int32_t kSegmentInset = 14;
static constexpr int32_t kDividerHeight = 28;
static constexpr int32_t kPlaylistHeaderHeight = 64;
static constexpr int32_t kPlaylistRowHeight = 72;
static constexpr int32_t kThumbSide = 56;
static constexpr ImageSize kThumbBox = { kThumbSide, kThumbSide };
static constexpr int32_t kThumbRadius = 6;
static constexpr std::size_t kRowLookahead = 2;
static constexpr int32_t kArtworkSide = 552;
static constexpr ImageSize kArtworkBox = { kArtworkSide, kArtworkSide };
static constexpr int32_t kArtworkRadius = 24;
/* LVGL draws through framebuffer 0 only. */
static constexpr int kArtworkFramebuffer = 1;

static constexpr uint32_t kForegroundColor = 0x101010;
static constexpr uint32_t kTrackColor = 0xd0d0d0;
static constexpr uint32_t kArtworkColor = 0xdcdcdc;
static constexpr uint32_t kArtworkIconColor = 0x9e9e9e;
static constexpr uint32_t kSubtitleColor = 0x707070;
static constexpr uint32_t kMessageColor = 0xc25e00;
static constexpr uint32_t kSegmentIconColor = 0x707070;
static constexpr uint32_t kPaneColor = 0xeeeeee;
static constexpr uint32_t kCurrentThumbColor = 0xcccccc;

static constexpr const char *kTitle = "Music Player";
static constexpr const char *kPaneIcons[] = {
    TABLER_PHOTO, TABLER_PLAYLIST, TABLER_INFO_CIRCLE, TABLER_ADJUSTMENTS_HORIZONTAL,
};
static constexpr int kPaneCount = sizeof(kPaneIcons) / sizeof(kPaneIcons[0]);

static int32_t body_line_height() {
    return lv_font_get_line_height(lv_widgets_resolved_font(LV_WIDGETS_FONT_BODY));
}

static std::string directory_name(const std::string &path) {
    const std::size_t slash = path.rfind('/');
    if (slash == std::string::npos || slash == 0) return {};
    const std::size_t start = path.rfind('/', slash - 1);
    return path.substr(start == std::string::npos ? 0 : start + 1, slash - start - 1);
}

static AudioPlayerScreen *s_active;

static lv_obj_t *create_row(lv_obj_t *parent, lv_flex_align_t main) {
    lv_obj_t *row = lv_container_create(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, main, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static lv_obj_t *create_column(lv_obj_t *parent) {
    lv_obj_t *column = lv_container_create(parent);
    lv_obj_set_size(column, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(column, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(column, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(column, kGap, 0);
    return column;
}

static std::string format_audio(CodecId codec, unsigned rate, unsigned channel_count) {
    if (codec == CodecId::None) return {};
    char text[64];
    const char *name = codec_name(codec);
    const char *channels = channel_count == 1   ? "mono"
                         : channel_count == 2   ? "stereo"
                                                : nullptr;
    if (!rate) {
        snprintf(text, sizeof(text), "%s", name);
    } else if (channels) {
        snprintf(text, sizeof(text), "%s  %.1f kHz  %s", name, rate / 1000.0, channels);
    } else {
        snprintf(text, sizeof(text), "%s  %.1f kHz  %uch", name, rate / 1000.0, channel_count);
    }
    return text;
}

static std::string format_summary(const MediaSummary &summary) {
    if (!summary.valid) return {};
    return format_audio(summary.audio.codec, summary.audio.sample_rate, summary.audio.channels);
}

static std::string format_names(const std::string &artist, const std::string &album) {
    if (artist.empty()) return album;
    if (album.empty()) return artist;
    return artist + " \u00b7 " + album;
}

static std::string format_tags(const MediaTags &tags) {
    return format_names(tags.artist, tags.album);
}

AudioPlayerScreen::AudioPlayerScreen(std::shared_ptr<Playlist> playlist)
    : playlist_(std::move(playlist)), repeat_(settings_audio_repeat()) {
    playlist_->setShuffled(settings_audio_shuffle());
    lv_anim_init(&marquee_);
    lv_anim_set_delay(&marquee_, kMarqueePauseMs);
    lv_anim_set_repeat_count(&marquee_, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_repeat_delay(&marquee_, kMarqueePauseMs);
}

void AudioPlayerScreen::build() {
    lv_obj_set_style_bg_color(root_, lv_color_white(), 0);
    lv_obj_set_style_text_color(root_, lv_color_hex(kForegroundColor), 0);
    lv_obj_add_event_fn(root_, LV_EVENT_SIZE_CHANGED, [this](lv_event_t *) {
        if (isLandscape() != landscape_) relayout();
    });
    buildContents();
}

bool AudioPlayerScreen::isLandscape() const {
    return lv_obj_get_width(root_) > lv_obj_get_height(root_);
}

void AudioPlayerScreen::relayout() {
    lv_async_call([this] {
        if (s_active == this) buildContents();
    });
}

void AudioPlayerScreen::buildContents() {
    lv_obj_clean(root_);
    segment_buttons_[0] = segment_buttons_[1] = segment_buttons_[2] = segment_buttons_[3] = nullptr;
    segment_dividers_[0] = segment_dividers_[1] = segment_dividers_[2] = nullptr;
    pane_box_ = nullptr;
    pane_view_ = nullptr;
    playlist_list_ = nullptr;
    playlist_count_ = nullptr;
    play_label_ = nullptr;
    next_button_ = nullptr;
    artwork_ = nullptr;
    artwork_icon_ = nullptr;
    artwork_image_ = nullptr;
    title_label_ = nullptr;
    repeat_button_ = nullptr;
    repeat_label_ = nullptr;
    shuffle_button_ = nullptr;
    seek_ = nullptr;
    elapsed_label_ = nullptr;
    total_label_ = nullptr;
    subtitle_label_ = nullptr;
    volume_label_ = nullptr;
    volume_slider_ = nullptr;
    scrubbing_ = false;
    shown_elapsed_s_ = -2;
    shown_total_s_ = -2;
    shown_title_.clear();
    shown_subtitle_.clear();

    landscape_ = isLandscape();
    if (landscape_) {
        buildLandscape();
    } else {
        buildPortrait();
    }
    showPane(pane_);

    setPlayIcon(playing_);
    setRepeatMode(repeat_);
    setShuffle(playlist_->shuffled());
    refresh();
}

void AudioPlayerScreen::buildPortrait() {
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_style_pad_row(root_, 0, 0);
    buildNavigation(root_);

    contents_ = lv_spacer_create(root_, lv_pct(100), LV_SIZE_CONTENT, 1);
    lv_obj_set_flex_flow(contents_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(contents_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(contents_, kPad, 0);
    lv_obj_update_layout(root_);

    const int32_t width = lv_obj_get_content_width(contents_);
    buildSegment(contents_, width - 2 * kSegmentInset);
    lv_spacer_create(contents_, 1, kSegmentGap);
    buildPane(contents_, width);

    lv_obj_t *area = lv_spacer_create(contents_, lv_pct(100), LV_SIZE_CONTENT, 1);
    buildControls(area, kPortraitTitleBottomPad);
}

void AudioPlayerScreen::buildLandscape() {
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_style_pad_right(root_, kPad, 0);
    lv_obj_set_style_pad_column(root_, 0, 0);

    lv_obj_t *left = lv_spacer_create(root_, LV_SIZE_CONTENT, lv_pct(100), 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    buildNavigation(left);
    contents_ = lv_spacer_create(left, lv_pct(100), LV_SIZE_CONTENT, 1);
    lv_obj_set_style_pad_hor(contents_, kPad, 0);
    buildControls(contents_, kLandscapeTitleBottomPad);

    lv_obj_t *right = lv_container_create(root_);
    lv_obj_set_size(right, kArtworkSide, lv_pct(100));
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(right, (kHeaderHeight - kSegmentHeight) / 2, 0);
    lv_obj_set_style_pad_bottom(right, kPad, 0);
    buildSegment(right, kArtworkSide);
    lv_spacer_create(right, 1, kSegmentGap);
    buildPane(right, kArtworkSide);
    lv_obj_set_flex_grow(pane_box_, 1);
}

void AudioPlayerScreen::buildNavigation(lv_obj_t *parent) {
    navigation_ = lv_navigation_create(parent, LV_NAVIGATION_STYLE_UNIFIED);
    lv_obj_t *back = lv_navigation_back_create(navigation_, kTitle,
                                               [this](lv_event_t *) { this->back(); });
    navigation_title_ = lv_navigation_back_label(back);
}

void AudioPlayerScreen::buildControls(lv_obj_t *parent, int32_t title_bottom_pad) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_t *controls = create_column(parent);
    buildTitle(controls, title_bottom_pad);
    buildSeekRow(controls);
    buildTransport(controls);
    buildVolumeRow(controls);
}

void AudioPlayerScreen::buildSegment(lv_obj_t *parent, int32_t width) {
    const lv_color_t foreground = lv_color_hex(kForegroundColor);
    const int32_t cell = width / kPaneCount;
    const int32_t left = (width - cell * kPaneCount) / 2;
    lv_obj_t *segment = lv_container_create(parent);
    lv_obj_set_size(segment, width, kSegmentHeight);
    lv_obj_set_scrollable(segment, false);

    for (int i = 0; i < kPaneCount; i++) {
        lv_obj_t *button = media_toggle_button(segment, kSegmentHeight, &icon_28, kPaneIcons[i],
                                               foreground, lv_color_white());
        lv_obj_set_size(button, cell, kSegmentHeight);
        lv_obj_set_pos(button, left + i * cell, 0);
        lv_obj_set_style_text_color(button, lv_color_hex(kSegmentIconColor), 0);
        lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [this, i](lv_event_t *) {
            if ((int)pane_ != i) showPane((Pane)i);
        });
        segment_buttons_[i] = button;
    }
    for (int i = 1; i < kPaneCount; i++) {
        lv_obj_t *divider = lv_container_create(segment, lv_color_hex(kTrackColor));
        lv_obj_set_size(divider, 1, kDividerHeight);
        lv_obj_set_pos(divider, left + i * cell, (kSegmentHeight - kDividerHeight) / 2);
        segment_dividers_[i - 1] = divider;
    }
}

void AudioPlayerScreen::updateSegment() {
    const int active = (int)pane_;
    for (int i = 0; i < kPaneCount; i++) {
        lv_obj_set_state(segment_buttons_[i], LV_STATE_CHECKED, i == active);
    }
    for (int i = 1; i < kPaneCount; i++) {
        lv_obj_set_hidden(segment_dividers_[i - 1], i == active || i - 1 == active);
    }
}

void AudioPlayerScreen::buildPane(lv_obj_t *parent, int32_t width) {
    pane_box_ = lv_container_create(parent);
    lv_obj_set_size(pane_box_, width, kArtworkSide);
    lv_obj_set_scrollable(pane_box_, false);
    lv_obj_align(buildArtwork(pane_box_, kArtworkSide), LV_ALIGN_BOTTOM_MID, 0, 0);
}

void AudioPlayerScreen::showPane(Pane pane) {
    pane_ = pane;
    updateSegment();
    if (pane_view_) {
        lv_obj_delete(pane_view_);
        pane_view_ = nullptr;
        playlist_list_ = nullptr;
        playlist_count_ = nullptr;
    }
    if (rows_token_) media_cache_cancel(rows_token_);
    requested_first_ = SIZE_MAX;
    lv_obj_set_hidden(artwork_, pane != Pane::Artwork);
    if (pane == Pane::Artwork) return;

    pane_view_ = lv_container_create(pane_box_, lv_color_hex(kPaneColor));
    lv_obj_set_size(pane_view_, lv_pct(100), lv_pct(100));
    lv_obj_set_style_radius(pane_view_, kArtworkRadius, 0);
    lv_obj_set_style_clip_corner(pane_view_, true, 0);
    lv_obj_set_flex_flow(pane_view_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(pane_view_, LV_DIR_VER);

    switch (pane) {
    case Pane::Playlist:
        buildPlaylist();
        break;
    case Pane::Info:
        buildInfo();
        break;
    case Pane::Settings:
        lv_setting_page_style(pane_view_);
        player_settings_sections_build(pane_view_, PlayerSettingsDisplay | PlayerSettingsSound,
                                       nullptr, false);
        break;
    case Pane::Artwork:
        break;
    }
}

void AudioPlayerScreen::buildInfo() {
    lv_obj_clean(pane_view_);
    lv_setting_page_style(pane_view_);
    const MediaSummary summary = awaiting_start_ ? MediaSummary{} : player_media_summary();
    info_valid_ = summary.valid;
    player_info_sections_build(pane_view_, name(), summary, nullptr);
}

void AudioPlayerScreen::buildPlaylist() {
    lv_obj_t *header = lv_container_create(pane_view_, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(header, kPlaylistHeaderHeight);
    lv_obj_set_style_pad_hor(header, kPad, 0);
    lv_obj_set_style_pad_column(header, kGap, 0);

    lv_obj_t *name = lv_label_create(header);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_set_height(name, body_line_height());
    lv_obj_set_font_role(name, LV_WIDGETS_FONT_BODY);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(name, directory_name(path()).c_str());

    playlist_count_ = lv_label_create(header);
    lv_obj_set_font_role(playlist_count_, LV_WIDGETS_FONT_BODY);
    lv_obj_set_style_text_color(playlist_count_, lv_color_hex(kSubtitleColor), 0);
    updatePlaylistCount();

    lv_hor_separator_create(pane_view_);
    shown_order_ = playlist_->orderVersion();
    shown_position_ = playlist_->position();
    playlist_list_ = lv_list_create(pane_view_, this);
    lv_obj_add_event_fn(playlist_list_, LV_EVENT_SCROLL, [this](lv_event_t *) { requestRows(); });
    lv_list_show_row(playlist_list_, playlist_->position());
    requestRows();
}

void AudioPlayerScreen::syncPlaylist() {
    if (!playlist_list_) return;
    updatePlaylistCount();
    const std::size_t position = playlist_->position();
    if (playlist_->orderVersion() != shown_order_) {
        shown_order_ = playlist_->orderVersion();
        shown_position_ = position;
        lv_list_reload_data(playlist_list_, true);
        lv_list_show_row(playlist_list_, position);
        requested_first_ = SIZE_MAX;
        requestRows();
        return;
    }
    if (position == shown_position_) return;
    const std::size_t previous = shown_position_;
    shown_position_ = position;
    rebindRow(previous);
    rebindRow(position);
}

void AudioPlayerScreen::rebindRow(std::size_t index) {
    if (lv_obj_t *row = lv_list_row_for_index(playlist_list_, index)) bindRow(row, index);
}

void AudioPlayerScreen::rebindPath(const std::string &path) {
    if (!playlist_list_) return;
    for (std::size_t i = 0; i < playlist_->size(); i++) {
        if (playlist_->at(playlist_->orderAt(i)).path != path) continue;
        rebindRow(i);
        return;
    }
}

void AudioPlayerScreen::updatePlaylistCount() {
    lv_label_set_text_fmt(playlist_count_, "%u/%u", (unsigned)playlist_->position() + 1,
                          (unsigned)playlist_->size());
}

bool AudioPlayerScreen::keepVisible(const char *path, void *ctx) {
    for (const std::string &kept : static_cast<AudioPlayerScreen *>(ctx)->visible_paths_) {
        if (kept == path) return true;
    }
    return false;
}

void AudioPlayerScreen::requestRows() {
    std::size_t first = lv_list_first_visible_row(playlist_list_);
    if (first == SIZE_MAX) first = 0;
    if (first == requested_first_) return;
    requested_first_ = first;

    const int32_t height = lv_obj_get_height(playlist_list_);
    const std::size_t rows = (std::size_t)(height / kPlaylistRowHeight) + 1;
    const std::size_t start = first > kRowLookahead ? first - kRowLookahead : 0;
    const std::size_t end = std::min(playlist_->size(), first + rows + kRowLookahead);
    visible_paths_.clear();
    for (std::size_t i = start; i < end; i++) {
        visible_paths_.push_back(playlist_->at(playlist_->orderAt(i)).path);
    }
    media_cache_retain(rows_token_, keepVisible, this);

    /* A request for what the cache already holds still completes, and every
     * completion reaches metaReady, so only the missing parts are asked for. */
    for (const std::string &row : visible_paths_) {
        if (media_cache_lookup(row)) continue;
        media_cache_request(row, MetaWantInfo, {}, MetaPriority::Visible, rows_token_);
    }
    for (const std::string &row : visible_paths_) {
        const std::shared_ptr<const MediaEntry> meta = media_cache_lookup(row);
        if (meta && meta->cover_scanned && !meta->has_cover) continue;
        if (media_cache_thumbnail(row, kThumbBox)) continue;
        media_cache_request(row, MetaWantInfo | MetaWantThumbnail, kThumbBox,
                            MetaPriority::Visible, rows_token_);
    }
}

lv_obj_t *AudioPlayerScreen::buildArtwork(lv_obj_t *parent, int32_t side) {
    artwork_ = lv_container_create(parent, lv_color_hex(kArtworkColor));
    artwork_side_ = side;
    lv_obj_set_size(artwork_, side, side);
    lv_obj_set_style_radius(artwork_, kArtworkRadius, 0);
    lv_obj_set_style_clip_corner(artwork_, true, 0);
    lv_obj_set_scrollable(artwork_, false);

    resetArtwork();
    applyArtwork();
    return artwork_;
}

void AudioPlayerScreen::resetArtwork() {
    if (!artwork_) return;
    if (artwork_image_) {
        lv_obj_delete(artwork_image_);
        artwork_image_ = nullptr;
    }
    if (artwork_icon_) return;

    artwork_icon_ = lv_label_create(artwork_);
    lv_label_set_text(artwork_icon_, TABLER_MUSIC);
    lv_obj_set_style_text_font(artwork_icon_, &icon_120, 0);
    lv_obj_set_style_text_color(artwork_icon_, lv_color_hex(kArtworkIconColor), 0);
    lv_obj_center(artwork_icon_);
}

void AudioPlayerScreen::applyArtwork() {
    if (!artwork_ || artwork_image_) return;
    auto *buffer = static_cast<uint8_t *>(bsp_display_get_frame_buffer(kArtworkFramebuffer));
    const bool rgb888 = bsp_display_get_pixel_format() == BSP_PIXEL_FORMAT_RGB888;
    if (!cover_size_.valid()) {
        const ImageSize box = { (int16_t)artwork_side_, (int16_t)artwork_side_ };
        const bsp_size_t panel = bsp_display_get_size();
        const std::size_t bytes = (std::size_t)panel.width * panel.height * (rgb888 ? 3 : 2);
        if (!media_cache_read_image(path(), box, buffer, bytes, &cover_size_)) return;
    }

    artwork_image_ = image_object_create(artwork_, buffer, cover_size_, rgb888);
    if (!artwork_image_) {
        cover_size_ = {};
        return;
    }
    if (artwork_icon_) {
        lv_obj_delete(artwork_icon_);
        artwork_icon_ = nullptr;
    }
}

std::string AudioPlayerScreen::currentTitle() const {
    if (meta_ && meta_->title[0]) return meta_->title;
    return name();
}

void AudioPlayerScreen::requestMeta() {
    meta_ = media_cache_resolve(path(), MetaWantInfo, {}, kResolveTimeoutMs);
    media_cache_request(path(), MetaWantInfo | MetaWantImage, kArtworkBox,
                        MetaPriority::Blocking, token_);
    prefetch_after_us_ = esp_timer_get_time() + kPrefetchDelayUs;
    prefetched_ = false;
}

void AudioPlayerScreen::prefetchNeighbours() {
    if (prefetched_ || prefetch_after_us_ == 0) return;
    if (!playing_ || esp_timer_get_time() < prefetch_after_us_) return;
    prefetched_ = true;

    const std::size_t count = playlist_->size();
    if (count < 2) return;
    const std::size_t next = playlist_->neighbour(1);
    const std::size_t previous = playlist_->neighbour(-1);

    media_cache_request(playlist_->at(next).path, MetaWantInfo | MetaWantImage, kArtworkBox,
                        MetaPriority::Idle, token_);
    if (previous != next) {
        media_cache_request(playlist_->at(previous).path, MetaWantInfo, {}, MetaPriority::Idle,
                            token_);
    }
}

void AudioPlayerScreen::metaReady(const std::string &path) {
    if (!s_active) return;
    s_active->rebindPath(path);
    if (s_active->path() != path) return;
    if (!s_active->meta_) s_active->meta_ = media_cache_lookup(path);
    s_active->applyArtwork();
    s_active->refresh();
}

void AudioPlayerScreen::buildTitle(lv_obj_t *parent, int32_t bottom_pad) {
    lv_obj_t *box = lv_container_create(parent);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, kTitleGap, 0);
    lv_obj_set_style_pad_bottom(box, bottom_pad, 0);

    title_label_ = lv_label_create(box);
    lv_obj_set_width(title_label_, lv_pct(100));
    lv_obj_set_height(title_label_,
                      lv_font_get_line_height(lv_widgets_resolved_font(LV_WIDGETS_FONT_TITLE)));
    lv_obj_set_font_role(title_label_, LV_WIDGETS_FONT_TITLE);
    lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_anim(title_label_, &marquee_, 0);
    shown_title_ = currentTitle();
    setScrollingText(title_label_, shown_title_);

    subtitle_label_ = lv_label_create(box);
    lv_obj_set_width(subtitle_label_, lv_pct(100));
    lv_obj_set_height(subtitle_label_,
                      lv_font_get_line_height(lv_widgets_resolved_font(LV_WIDGETS_FONT_BODY)));
    lv_obj_set_font_role(subtitle_label_, LV_WIDGETS_FONT_BODY);
    lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(kSubtitleColor), 0);
    lv_obj_set_style_anim(subtitle_label_, &marquee_, 0);
    setScrollingText(subtitle_label_, "");
}

void AudioPlayerScreen::setScrollingText(lv_obj_t *label, const std::string &text) {
    /* Setting the mode again restarts the scroll from the start; a plain
     * set_text keeps the old offset. */
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_label_set_text(label, text.c_str());
}

void AudioPlayerScreen::buildSeekRow(lv_obj_t *parent) {
    lv_obj_t *row = create_row(parent, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_height(row, kSeekRowHeight);

    elapsed_label_ = lv_label_create(row);
    lv_obj_set_width(elapsed_label_, kTimeWidth);
    lv_obj_set_style_text_align(elapsed_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_font_role(elapsed_label_, LV_WIDGETS_FONT_BODY);

    seek_ = media_slider(row, kSeekRange, lv_color_hex(kForegroundColor),
                         lv_color_hex(kTrackColor));
    lv_obj_set_flex_grow(seek_, 1);
    lv_obj_add_event_fn(seek_, LV_EVENT_PRESSED, [this](lv_event_t *) { scrubbing_ = true; });
    lv_obj_add_event_fn(seek_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t *) {
        if (!scrubbing_) return;
        const PlayerStatus status = player_status();
        if (status.duration_us <= 0) return;
        setTime(elapsed_label_, &shown_elapsed_s_,
                (int64_t)lv_slider_get_value(seek_) * status.duration_us / kSeekRange);
    });
    lv_obj_add_event_fn(seek_, LV_EVENT_RELEASED, [this](lv_event_t *) {
        scrubbing_ = false;
        const PlayerStatus status = player_status();
        if (!status.seekable || status.duration_us <= 0) return;
        player_seek((int64_t)lv_slider_get_value(seek_) * status.duration_us / kSeekRange);
    });

    total_label_ = lv_label_create(row);
    lv_obj_set_width(total_label_, kTimeWidth);
    lv_obj_set_style_text_align(total_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_font_role(total_label_, LV_WIDGETS_FONT_BODY);
}

void AudioPlayerScreen::buildTransport(lv_obj_t *parent) {
    const lv_color_t foreground = lv_color_hex(kForegroundColor);
    lv_obj_t *outer = create_row(parent, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(outer, 0, 0);

    lv_obj_t *left = lv_container_create(outer, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(left, kTimeWidth, LV_SIZE_CONTENT);
    shuffle_button_ = media_toggle_button(left, kIconButton, &icon_36, TABLER_ARROWS_SHUFFLE,
                                          foreground, lv_color_white());
    lv_obj_add_event_fn(shuffle_button_, LV_EVENT_CLICKED, [this](lv_event_t *) {
        setShuffle(!playlist_->shuffled());
        settings_set_audio_shuffle(playlist_->shuffled());
        settings_commit();
    });

    lv_obj_t *row = create_row(outer, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_width(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(row, 16, 0);

    lv_obj_t *prev = media_icon_button(row, 96, &icon_48, TABLER_PLAYER_TRACK_PREV, foreground);
    lv_obj_add_event_fn(prev, LV_EVENT_CLICKED, [this](lv_event_t *) {
        const PlayerStatus status = player_status();
        if (status.position_us < kPrevTrackUs && advance(-1, true)) return;
        restart(status.state == PlayerState::Playing || status.state == PlayerState::Finished);
    });

    lv_obj_t *play =
        media_icon_button(row, 120, &icon_72, TABLER_PLAYER_PLAY, foreground, &play_label_);
    lv_obj_add_event_fn(play, LV_EVENT_CLICKED, [this](lv_event_t *) {
        if (!awaiting_start_) {
            if (playing_) {
                player_pause();
            } else {
                player_play();
            }
        }
        setPlayIcon(!playing_);
    });

    next_button_ = media_icon_button(row, 96, &icon_48, TABLER_PLAYER_TRACK_NEXT, foreground);
    lv_obj_add_event_fn(next_button_, LV_EVENT_CLICKED, [this](lv_event_t *) { advance(1, true); });

    lv_obj_t *side = lv_container_create(outer, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(side, kTimeWidth, LV_SIZE_CONTENT);
    repeat_button_ = media_toggle_button(side, kIconButton, &icon_36, TABLER_REPEAT, foreground,
                                         lv_color_white(), &repeat_label_);
    lv_obj_add_event_fn(repeat_button_, LV_EVENT_CLICKED, [this](lv_event_t *) {
        switch (repeat_) {
        case RepeatMode::Off: setRepeatMode(RepeatMode::All); break;
        case RepeatMode::All: setRepeatMode(RepeatMode::One); break;
        case RepeatMode::One: setRepeatMode(RepeatMode::Off); break;
        }
        player_set_loop(repeat_ == RepeatMode::One);
        settings_set_audio_repeat(repeat_);
        settings_commit();
    });
}

void AudioPlayerScreen::buildVolumeRow(lv_obj_t *parent) {
    const lv_color_t foreground = lv_color_hex(kForegroundColor);
    lv_obj_t *row = create_row(parent, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 16, 0);

    lv_obj_t *left = lv_container_create(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(left, kTimeWidth, LV_SIZE_CONTENT);
    lv_obj_t *mute = media_icon_button(left, kIconButton, &icon_36, TABLER_VOLUME, foreground,
                                       &volume_label_);

    volume_slider_ = media_slider(row, 100, foreground, lv_color_hex(kTrackColor));
    lv_obj_set_flex_grow(volume_slider_, 1);
    media_volume_bind(mute, volume_label_, volume_slider_);

    lv_spacer_create(row, kTimeWidth, 1);
}

void AudioPlayerScreen::openCurrent() {
    awaiting_start_ = true;
    scrubbing_ = false;
    cover_size_ = {};
    meta_ = nullptr;
    resetArtwork();
    requestMeta();
    player_open(path());
    player_set_loop(repeat_ == RepeatMode::One);

    shown_elapsed_s_ = -2;
    shown_total_s_ = -2;
    if (title_label_) {
        shown_title_ = currentTitle();
        setScrollingText(title_label_, shown_title_);
    }
    if (subtitle_label_) {
        shown_subtitle_.clear();
        setScrollingText(subtitle_label_, "");
        lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(kSubtitleColor), 0);
    }
    if (pane_ == Pane::Info && pane_view_) buildInfo();
    syncPlaylist();
    updateTransport();
    setPlayIcon(true);
    refresh();
}

bool AudioPlayerScreen::advance(int delta, bool manual) {
    const std::size_t before = playlist_->index();
    if (!playlist_->step(delta, repeat_)) return false;
    auto_opened_ = !manual;
    if (manual) skips_ = 0;
    if (playlist_->index() == before) {
        restart(true);
        return true;
    }
    openCurrent();
    return true;
}

void AudioPlayerScreen::restart(bool resume) {
    player_restart();
    if (!resume) return;
    player_play();
    setPlayIcon(true);
}

void AudioPlayerScreen::updateTransport() {
    if (!next_button_) return;
    lv_obj_set_state(next_button_, LV_STATE_DISABLED, !playlist_->canStep(1, repeat_));
}

void AudioPlayerScreen::playerStateChanged() {
    if (s_active) s_active->handleState();
}

void AudioPlayerScreen::handleState() {
    prefetchNeighbours();
    const PlayerStatus status = player_status();
    if (awaiting_start_) {
        if (status.state == PlayerState::Paused) {
            awaiting_start_ = false;
            skips_ = 0;
            if (playing_) {
                player_play();
                /* The status still says paused, so refreshing here would flip
                 * the icon back. The Playing transition brings the next one. */
                return;
            }
        }
        if (status.state == PlayerState::Failed) {
            awaiting_start_ = false;
            if (auto_opened_ && ++skips_ < (int)playlist_->size() && advance(1, false)) return;
        }
    } else if (status.state == PlayerState::Finished && advance(1, false)) {
        return;
    }
    refresh();
}

void AudioPlayerScreen::setRepeatMode(RepeatMode mode) {
    repeat_ = mode;
    updateTransport();
    if (!repeat_button_) return;
    lv_label_set_text(repeat_label_, mode == RepeatMode::One ? TABLER_REPEAT_ONCE : TABLER_REPEAT);
    lv_obj_set_state(repeat_button_, LV_STATE_CHECKED, mode != RepeatMode::Off);
}

void AudioPlayerScreen::setShuffle(bool shuffled) {
    playlist_->setShuffled(shuffled);
    syncPlaylist();
    updateTransport();
    if (shuffle_button_) lv_obj_set_state(shuffle_button_, LV_STATE_CHECKED, shuffled);
}

void AudioPlayerScreen::setPlayIcon(bool playing) {
    playing_ = playing;
    if (play_label_) {
        lv_label_set_text(play_label_, playing ? TABLER_PLAYER_PAUSE : TABLER_PLAYER_PLAY);
    }
}

void AudioPlayerScreen::setTime(lv_obj_t *label, int64_t *shown_s, int64_t us) {
    const int64_t seconds = us < 0 ? -1 : us / 1000000;
    if (seconds == *shown_s) return;
    *shown_s = seconds;
    char text[16];
    media_format_time(text, sizeof(text), seconds);
    lv_label_set_text(label, text);
}

void AudioPlayerScreen::refresh() {
    if (!seek_) return;

    /* Until the file the player was told to open is loaded, its status and
     * summary still describe the previous one. */
    const bool ready = !awaiting_start_;
    const PlayerStatus status = player_status();
    const bool playing = ready ? status.state == PlayerState::Playing : playing_;
    if (playing != playing_) setPlayIcon(playing);

    const bool known = ready && status.duration_us > 0 && status.state != PlayerState::Loading;
    setTime(total_label_, &shown_total_s_, known ? status.duration_us : -1);
    if (!scrubbing_) {
        setTime(elapsed_label_, &shown_elapsed_s_, known ? status.position_us : 0);
        const int64_t value = known ? status.position_us * kSeekRange / status.duration_us : 0;
        lv_slider_set_value(seek_, (int32_t)value, LV_ANIM_OFF);
    }

    const MediaSummary summary = ready ? player_media_summary() : MediaSummary{};
    applyArtwork();

    const std::string title = currentTitle();
    if (title != shown_title_) {
        shown_title_ = title;
        setScrollingText(title_label_, title);
    }

    std::string message;
    if (ready) {
        if (status.state == PlayerState::Failed) message = status.error;
        if (message.empty()) message = status.audio_note;
    }
    const bool failed = !message.empty();
    if (message.empty() && meta_) message = format_names(meta_->artist, meta_->album);
    if (message.empty()) message = format_tags(summary.tags);
    if (message.empty() && meta_) {
        message = format_audio(meta_->audio_codec, meta_->sample_rate, meta_->channels);
    }
    if (message.empty()) message = format_summary(summary);
    if (pane_ == Pane::Info && pane_view_ && summary.valid != info_valid_) buildInfo();
    if (message != shown_subtitle_) {
        shown_subtitle_ = message;
        setScrollingText(subtitle_label_, message);
        lv_obj_set_style_text_color(
            subtitle_label_, lv_color_hex(failed ? kMessageColor : kSubtitleColor), 0);
    }
}

std::size_t AudioPlayerScreen::rowCount() const {
    return playlist_->size();
}

int32_t AudioPlayerScreen::rowHeight() const {
    return kPlaylistRowHeight;
}

lv_obj_t *AudioPlayerScreen::createRow(lv_obj_t *parent) {
    lv_obj_t *row = lv_list_row_create(parent);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_hor(row, kPad, 0);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_style_bg_color(row, lv_color_hex(kArtworkColor), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);

    lv_obj_t *frame = lv_container_create(row, lv_color_hex(kArtworkColor));
    lv_obj_set_size(frame, kThumbSide, kThumbSide);
    lv_obj_set_style_radius(frame, kThumbRadius, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_clickable(frame, false);
    lv_obj_set_scrollable(frame, false);
    lv_obj_t *icon = lv_label_create(frame);
    lv_label_set_text(icon, TABLER_MUSIC);
    lv_obj_set_style_text_font(icon, &icon_28, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(kArtworkIconColor), 0);
    lv_obj_center(icon);

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
        if (i) lv_obj_set_style_text_color(label, lv_color_hex(kSubtitleColor), 0);
    }
    return row;
}

void AudioPlayerScreen::bindRow(lv_obj_t *row, std::size_t index) {
    const PlaylistItem &item = playlist_->at(playlist_->orderAt(index));
    const std::shared_ptr<const MediaEntry> meta = media_cache_lookup(item.path);
    const std::string names = meta ? format_names(meta->artist, meta->album) : std::string();
    lv_obj_t *text = lv_obj_get_child(row, 1);
    lv_obj_t *subtitle = lv_obj_get_child(text, 1);
    lv_label_set_text(lv_obj_get_child(text, 0),
                      meta && meta->title[0] ? meta->title : item.name.c_str());
    lv_label_set_text(subtitle, names.c_str());
    lv_obj_set_hidden(subtitle, names.empty());
    const bool current = index == playlist_->position();
    lv_obj_set_state(row, LV_STATE_CHECKED, current);
    lv_obj_set_clickable(row, true);

    lv_obj_t *frame = lv_obj_get_child(row, 0);
    lv_obj_set_style_bg_color(frame, lv_color_hex(current ? kCurrentThumbColor : kArtworkColor), 0);
    std::shared_ptr<const ImagePixels> pixels = media_cache_thumbnail(item.path, kThumbBox);
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
            lv_obj_set_size(image, kThumbSide, kThumbSide);
            lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
        }
    }
    lv_obj_set_hidden(lv_obj_get_child(frame, 0), image != nullptr);
}

void AudioPlayerScreen::didSelectRow(std::size_t index) {
    if (index == playlist_->position()) return;
    playlist_->jumpTo(index);
    auto_opened_ = false;
    skips_ = 0;
    openCurrent();
}

void AudioPlayerScreen::onEnter() {
    s_active = this;
    media_cache_idle_cancel();
    if (!token_) token_ = media_cache_token();
    if (!rows_token_) rows_token_ = media_cache_token();
    media_cache_observe(token_, metaReady);
    player_observe_state(playerStateChanged);
    openCurrent();
    timer_ = lv_timer_create([](lv_timer_t *timer) {
        static_cast<AudioPlayerScreen *>(lv_timer_get_user_data(timer))->handleState();
    }, kRefreshPeriodMs, this);
}

void AudioPlayerScreen::onExit() {
    if (s_active == this) s_active = nullptr;
    media_cache_unobserve(token_);
    media_cache_cancel(token_);
    media_cache_cancel(rows_token_);
    player_observe_state(nullptr);
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    player_close();
}

AudioPlayerScreen::~AudioPlayerScreen() {
    if (timer_) lv_timer_delete(timer_);
}

void AudioPlayerScreen::eject(const std::string &mount_point) {
    if (s_active && path_is_under(s_active->path(), mount_point)) s_active->back();
}
