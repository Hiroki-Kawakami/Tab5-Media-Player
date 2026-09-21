/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "audio_player_screen.hpp"
#include "media_player.hpp"
#include "playback/player.hpp"
#include "screens/media_controls.hpp"
#include "resources.h"

#include <algorithm>
#include <cstdio>

static constexpr uint32_t kRefreshPeriodMs = 500;
static constexpr uint32_t kAutoStartPollMs = 100;
static constexpr int32_t kSeekRange = 1000;
static constexpr int32_t kPad = 24;
static constexpr int32_t kGap = 24;
static constexpr int32_t kTimeWidth = 100;
static constexpr int32_t kIconButton = 72;
static constexpr int32_t kMinArtwork = 160;
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

static std::string format_summary(const MediaSummary &summary) {
    if (!summary.valid || summary.audio.codec == CodecId::None) return {};
    char text[64];
    const char *codec = codec_name(summary.audio.codec);
    const unsigned rate = summary.audio.sample_rate;
    const char *channels = summary.audio.channels == 1   ? "mono"
                         : summary.audio.channels == 2   ? "stereo"
                                                         : nullptr;
    if (!rate) {
        snprintf(text, sizeof(text), "%s", codec);
    } else if (channels) {
        snprintf(text, sizeof(text), "%s  %.1f kHz  %s", codec, rate / 1000.0, channels);
    } else {
        snprintf(text, sizeof(text), "%s  %.1f kHz  %uch", codec, rate / 1000.0,
                 (unsigned)summary.audio.channels);
    }
    return text;
}

void AudioPlayerScreen::build() {
    createNavigation(name_.c_str(), LV_NAVIGATION_STYLE_DEFAULT | LV_NAVIGATION_STYLE_BACK);
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
    shown_subtitle_.clear();

    landscape_ = isLandscape();
    lv_obj_set_style_pad_all(contents_, kPad, 0);
    lv_obj_set_style_pad_row(contents_, kGap, 0);
    lv_obj_set_style_pad_column(contents_, kGap, 0);
    lv_obj_set_style_text_color(contents_, lv_color_hex(kForegroundColor), 0);
    lv_obj_set_flex_flow(contents_, landscape_ ? LV_FLEX_FLOW_ROW : LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(contents_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_update_layout(root_);

    const int32_t width = lv_obj_get_width(contents_) - 2 * kPad;
    const int32_t height = lv_obj_get_height(contents_) - 2 * kPad;
    lv_obj_t *controls = create_column(contents_);
    lv_obj_set_height(controls, LV_SIZE_CONTENT);

    if (landscape_) {
        const int32_t side = std::max(std::min(height, width / 2), kMinArtwork);
        lv_obj_set_width(controls, width - side - kGap);
        buildTitle(controls);
        buildSeekRow(controls);
        buildTransport(controls);
        buildVolumeRow(controls);
        buildArtwork(contents_, side);
    } else {
        lv_obj_set_width(controls, width);
        buildTitle(controls);
        buildSeekRow(controls);
        buildTransport(controls);
        buildVolumeRow(controls);
        lv_obj_update_layout(contents_);
        const int32_t room = height - lv_obj_get_height(controls) - kGap;
        lv_obj_t *artwork = buildArtwork(contents_, std::clamp(room, kMinArtwork, width));
        lv_obj_move_to_index(artwork, 0);
    }

    setPlayIcon(playing_);
    setRepeatMode(repeat_);
    refresh();
}

lv_obj_t *AudioPlayerScreen::buildArtwork(lv_obj_t *parent, int32_t side) {
    lv_obj_t *artwork = lv_container_create(parent, lv_color_hex(kArtworkColor));
    lv_obj_set_size(artwork, side, side);
    lv_obj_set_style_radius(artwork, kArtworkRadius, 0);
    lv_obj_remove_flag(artwork, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *icon = lv_label_create(artwork);
    lv_label_set_text(icon, TABLER_MUSIC);
    lv_obj_set_style_text_font(icon, &icon_120, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(kArtworkIconColor), 0);
    lv_obj_center(icon);
    return artwork;
}

void AudioPlayerScreen::buildTitle(lv_obj_t *parent) {
    lv_obj_t *box = lv_container_create(parent);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, 8, 0);

    lv_obj_t *title = lv_label_create(box);
    lv_obj_set_width(title, lv_pct(100));
    lv_obj_set_style_text_font(title, lv_widgets_title_font(), 0);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(title, name_.c_str());

    subtitle_label_ = lv_label_create(box);
    lv_obj_set_width(subtitle_label_, lv_pct(100));
    lv_obj_set_height(subtitle_label_, lv_font_get_line_height(lv_widgets_body_font()));
    lv_obj_set_style_text_font(subtitle_label_, lv_widgets_body_font(), 0);
    lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(kSubtitleColor), 0);
    lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(subtitle_label_, "");
}

void AudioPlayerScreen::buildSeekRow(lv_obj_t *parent) {
    lv_obj_t *row = create_row(parent, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 16, 0);

    elapsed_label_ = lv_label_create(row);
    lv_obj_set_width(elapsed_label_, kTimeWidth);
    lv_obj_set_style_text_align(elapsed_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(elapsed_label_, lv_widgets_body_font(), 0);

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
    lv_obj_set_style_text_font(total_label_, lv_widgets_body_font(), 0);
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
    lv_obj_add_event_fn(prev, LV_EVENT_CLICKED, [](lv_event_t *) { player_restart(); });

    lv_obj_t *play =
        media_icon_button(row, 120, &icon_72, TABLER_PLAYER_PLAY, foreground, &play_label_);
    lv_obj_add_event_fn(play, LV_EVENT_CLICKED, [this](lv_event_t *) {
        if (playing_) {
            player_pause();
        } else {
            player_play();
        }
        setPlayIcon(!playing_);
    });

    lv_obj_t *next = media_icon_button(row, 96, &icon_48, TABLER_PLAYER_TRACK_NEXT, foreground);
    lv_obj_add_state(next, LV_STATE_DISABLED);

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
        player_set_loop(repeat_ != RepeatMode::Off);
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

void AudioPlayerScreen::setRepeatMode(RepeatMode mode) {
    repeat_ = mode;
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

void AudioPlayerScreen::tick() {
    const PlayerState state = player_status().state;
    if (auto_start_ && (state == PlayerState::Paused || state == PlayerState::Failed)) {
        auto_start_ = false;
        lv_timer_set_period(timer_, kRefreshPeriodMs);
        if (state == PlayerState::Paused) {
            player_play();
            setPlayIcon(true);
        }
    }
    refresh();
}

void AudioPlayerScreen::refresh() {
    if (!seek_) return;

    const PlayerStatus status = player_status();
    const bool playing = status.state == PlayerState::Playing;
    if (playing != playing_) setPlayIcon(playing);

    const bool known = status.duration_us > 0 && status.state != PlayerState::Loading;
    setTime(total_label_, &shown_total_s_, known ? status.duration_us : -1);
    if (!scrubbing_) {
        setTime(elapsed_label_, &shown_elapsed_s_, known ? status.position_us : 0);
        const int64_t value = known ? status.position_us * kSeekRange / status.duration_us : 0;
        lv_slider_set_value(seek_, (int32_t)value, LV_ANIM_OFF);
    }

    std::string message = status.state == PlayerState::Failed ? status.error : std::string();
    if (message.empty()) message = status.audio_note;
    const bool failed = !message.empty();
    if (message.empty()) message = format_summary(player_media_summary());
    if (message != shown_subtitle_) {
        shown_subtitle_ = message;
        lv_label_set_text(subtitle_label_, message.c_str());
        lv_obj_set_style_text_color(
            subtitle_label_, lv_color_hex(failed ? kMessageColor : kSubtitleColor), 0);
    }
}

void AudioPlayerScreen::onEnter() {
    s_active = this;
    player_open(path_);
    player_set_loop(repeat_ != RepeatMode::Off);

    auto_start_ = true;
    timer_ = lv_timer_create([](lv_timer_t *timer) {
        static_cast<AudioPlayerScreen *>(lv_timer_get_user_data(timer))->tick();
    }, kAutoStartPollMs, this);
    refresh();
}

void AudioPlayerScreen::onExit() {
    if (s_active == this) s_active = nullptr;
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
    if (s_active && path_is_under(s_active->path_, mount_point)) s_active->back();
}
