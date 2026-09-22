/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "audio_player_screen.hpp"
#include "media_player.hpp"
#include "screens/image_object.hpp"
#include "screens/media_controls.hpp"
#include "resources.h"
#include "esp_timer.h"

#include <cstdio>

static constexpr uint32_t kRefreshPeriodMs = 500;
static constexpr uint32_t kResolveTimeoutMs = 700;
static constexpr int64_t kPrefetchDelayUs = 2000000;
static constexpr int64_t kPrevTrackUs = 3000000;
static constexpr int32_t kSeekRange = 1000;
static constexpr int32_t kPad = 24;
static constexpr int32_t kGap = 24;
static constexpr int32_t kTimeWidth = 100;
static constexpr int32_t kTitleBottomPad = 40;
static constexpr int32_t kArtworkGap = 64;
static constexpr int32_t kSeekRowHeight = 56;
static constexpr int32_t kIconButton = 72;
static constexpr int32_t kArtworkSide = 552;
static constexpr ImageBox kArtworkBox = { kArtworkSide, kArtworkSide };
static constexpr int32_t kArtworkRadius = 24;

static constexpr uint32_t kForegroundColor = 0x101010;
static constexpr uint32_t kTrackColor = 0xd0d0d0;
static constexpr uint32_t kArtworkColor = 0xdcdcdc;
static constexpr uint32_t kArtworkIconColor = 0x9e9e9e;
static constexpr uint32_t kSubtitleColor = 0x707070;
static constexpr uint32_t kMessageColor = 0xc25e00;

static AudioPlayerScreen *s_active;

static lv_obj_t *create_row(lv_obj_t *parent, lv_flex_align_t main) {
    lv_obj_t *row = lv_container_create(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, main, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

static lv_obj_t *create_column(lv_obj_t *parent) {
    lv_obj_t *column = lv_container_create(parent);
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
    return artist + " - " + album;
}

static std::string format_tags(const MediaTags &tags) {
    return format_names(tags.artist, tags.album);
}

void AudioPlayerScreen::build() {
    createNavigation(name().c_str(), LV_NAVIGATION_STYLE_DEFAULT | LV_NAVIGATION_STYLE_BACK);
    lv_obj_set_style_bg_color(root_, lv_color_white(), 0);
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
    lv_obj_clean(contents_);
    play_label_ = nullptr;
    next_button_ = nullptr;
    artwork_ = nullptr;
    artwork_icon_ = nullptr;
    artwork_image_ = nullptr;
    title_label_ = nullptr;
    repeat_label_ = nullptr;
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
    lv_obj_set_style_pad_all(contents_, kPad, 0);
    lv_obj_set_style_pad_row(contents_, kArtworkGap, 0);
    lv_obj_set_style_pad_column(contents_, kGap, 0);
    lv_obj_set_style_text_color(contents_, lv_color_hex(kForegroundColor), 0);
    lv_obj_set_flex_flow(contents_, landscape_ ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(contents_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_update_layout(root_);

    const int32_t width = lv_obj_get_width(contents_) - 2 * kPad;
    lv_obj_t *controls = create_column(contents_);
    lv_obj_set_height(controls, LV_SIZE_CONTENT);

    lv_obj_set_width(controls, landscape_ ? width - kArtworkSide - kGap : width);
    buildTitle(controls);
    buildSeekRow(controls);
    buildTransport(controls);
    buildVolumeRow(controls);

    lv_obj_t *artwork = buildArtwork(contents_, kArtworkSide);
    if (!landscape_) lv_obj_move_to_index(artwork, 0);

    setPlayIcon(playing_);
    setRepeatMode(repeat_);
    refresh();
}

lv_obj_t *AudioPlayerScreen::buildArtwork(lv_obj_t *parent, int32_t side) {
    artwork_ = lv_container_create(parent, lv_color_hex(kArtworkColor));
    artwork_side_ = side;
    lv_obj_set_size(artwork_, side, side);
    lv_obj_set_style_radius(artwork_, kArtworkRadius, 0);
    lv_obj_set_style_clip_corner(artwork_, true, 0);
    lv_obj_remove_flag(artwork_, LV_OBJ_FLAG_SCROLLABLE);

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
    const ImageBox box = { (int16_t)artwork_side_, (int16_t)artwork_side_ };
    if (!cover_) cover_ = media_cache_image(path(), box);
    if (!cover_) return;

    artwork_image_ = image_object_create(artwork_, cover_);
    if (!artwork_image_) {
        cover_ = {};
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
    const std::size_t index = playlist_->index();
    const std::size_t next = (index + 1) % count;
    const std::size_t previous = (index + count - 1) % count;

    media_cache_request(playlist_->at(next).path, MetaWantInfo | MetaWantImage, kArtworkBox,
                        MetaPriority::Idle, token_);
    if (previous != next) {
        media_cache_request(playlist_->at(previous).path, MetaWantInfo, {}, MetaPriority::Idle,
                            token_);
    }
}

void AudioPlayerScreen::metaReady(const std::string &path) {
    if (!s_active || s_active->path() != path) return;
    if (!s_active->meta_) s_active->meta_ = media_cache_lookup(path);
    s_active->applyArtwork();
    s_active->refresh();
}

void AudioPlayerScreen::buildTitle(lv_obj_t *parent) {
    lv_obj_t *box = lv_container_create(parent);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 8, 0);
    lv_obj_set_style_pad_bottom(box, kTitleBottomPad, 0);

    title_label_ = lv_label_create(box);
    lv_obj_set_width(title_label_, lv_pct(100));
    lv_obj_set_font_role(title_label_, LV_WIDGETS_FONT_TITLE);
    lv_obj_set_style_text_align(title_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title_label_, LV_LABEL_LONG_MODE_DOTS);
    shown_title_ = currentTitle();
    lv_label_set_text(title_label_, shown_title_.c_str());

    subtitle_label_ = lv_label_create(box);
    lv_obj_set_width(subtitle_label_, lv_pct(100));
    lv_obj_set_height(subtitle_label_, lv_font_get_line_height(lv_widgets_resolved_font(LV_WIDGETS_FONT_BODY)));
    lv_obj_set_font_role(subtitle_label_, LV_WIDGETS_FONT_BODY);
    lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(kSubtitleColor), 0);
    lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(subtitle_label_, "");
}

void AudioPlayerScreen::buildSeekRow(lv_obj_t *parent) {
    lv_obj_t *row = create_row(parent, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_style_min_height(row, kSeekRowHeight, 0);

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
    lv_spacer_create(outer, kTimeWidth, 1);

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
    lv_obj_t *repeat = media_icon_button(side, kIconButton, &icon_36, TABLER_REPEAT_OFF,
                                         foreground, &repeat_label_);
    lv_obj_add_event_fn(repeat, LV_EVENT_CLICKED, [this](lv_event_t *) {
        switch (repeat_) {
        case RepeatMode::Off: setRepeatMode(RepeatMode::All); break;
        case RepeatMode::All: setRepeatMode(RepeatMode::One); break;
        case RepeatMode::One: setRepeatMode(RepeatMode::Off); break;
        }
        player_set_loop(repeat_ == RepeatMode::One);
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
    cover_ = {};
    meta_ = nullptr;
    resetArtwork();
    requestMeta();
    player_open(path());
    player_set_loop(repeat_ == RepeatMode::One);

    lv_label_set_text(navigation_title_, name().c_str());
    shown_elapsed_s_ = -2;
    shown_total_s_ = -2;
    if (title_label_) {
        shown_title_ = currentTitle();
        lv_label_set_text(title_label_, shown_title_.c_str());
    }
    if (subtitle_label_) {
        shown_subtitle_.clear();
        lv_label_set_text(subtitle_label_, "");
        lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(kSubtitleColor), 0);
    }
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
    if (!repeat_label_) return;
    switch (mode) {
    case RepeatMode::Off: lv_label_set_text(repeat_label_, TABLER_REPEAT_OFF); break;
    case RepeatMode::All: lv_label_set_text(repeat_label_, TABLER_REPEAT); break;
    case RepeatMode::One: lv_label_set_text(repeat_label_, TABLER_REPEAT_ONCE); break;
    }
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
        lv_label_set_text(title_label_, title.c_str());
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
    if (message != shown_subtitle_) {
        shown_subtitle_ = message;
        lv_label_set_text(subtitle_label_, message.c_str());
        lv_obj_set_style_text_color(
            subtitle_label_, lv_color_hex(failed ? kMessageColor : kSubtitleColor), 0);
    }
}

void AudioPlayerScreen::onEnter() {
    s_active = this;
    media_cache_idle_cancel();
    if (!token_) token_ = media_cache_token();
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
