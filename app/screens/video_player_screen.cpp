/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "video_player_screen.hpp"
#include "media/media_cache.hpp"
#include "media_player.hpp"
#include "playback/player.hpp"
#include "screens/media_controls.hpp"
#include "screens/video_player/info_panel.hpp"
#include "screens/video_player/settings_panel.hpp"
#include "settings.hpp"
#include "video/video_presenter.hpp"
#include "ui_orientation.hpp"
#include "bsp.h"
#include "display_manager.hpp"
#include "resources.h"

#include <cstdio>

static constexpr int32_t kTopBarHeight = 80;
static constexpr int32_t kLandscapeBottomHeight = 160;
static constexpr int32_t kPortraitBottomHeight = 272;
static constexpr int32_t kPortraitPanelHeight = 640;
static constexpr int32_t kLandscapePanelWidth = 560;
static constexpr uint32_t kRefreshPeriodMs = 500;
static constexpr uint32_t kAutoHideMs = 4000;
static constexpr uint32_t kAutoStartHideMs = 1500;
static constexpr int32_t kSeekRange = 1000;
static constexpr int64_t kPrevTrackUs = 3000000;
static constexpr int32_t kBarPadding = 24;
static constexpr int32_t kPortraitSide = 116;
static constexpr int32_t kPortraitSlider = 440;
static constexpr int32_t kTimeWidth = 100;
static constexpr int32_t kIconButton = 72;
static constexpr int32_t kVolumeSlider = 280;
static constexpr int32_t kSeekGap = 16;
static constexpr int32_t kOverlayBufferLines = 32;

static constexpr uint32_t kBarColor = 0x101010;
static constexpr uint32_t kTrackColor = 0x404040;
static constexpr uint32_t kMessageColor = 0xffb74d;

/* Clockwise, so a rotation is a shift along the cycle. */
enum class Edge { Top, Right, Bottom, Left };

static VideoPlayerScreen *s_active;

static bool is_portrait(bsp_rotation_t rotation) {
    return rotation == BSP_ROTATION_0 || rotation == BSP_ROTATION_180;
}

static int32_t bottom_height(bsp_rotation_t rotation) {
    return is_portrait(rotation) ? kPortraitBottomHeight : kLandscapeBottomHeight;
}

static Edge panel_edge(Edge edge, bsp_rotation_t rotation) {
    return (Edge)(((int)edge - (int)rotation + 4) % 4);
}

static void add_inset(VideoInsets &insets, Edge edge, int32_t thickness) {
    switch (edge) {
    case Edge::Top:    insets.top = thickness; break;
    case Edge::Right:  insets.right = thickness; break;
    case Edge::Bottom: insets.bottom = thickness; break;
    default:           insets.left = thickness; break;
    }
}

static lv_obj_t *create_bar(lv_obj_t *parent, int32_t width, int32_t height, lv_align_t align) {
    lv_obj_t *bar = lv_container_create(parent, lv_color_hex(kBarColor));
    lv_obj_set_size(bar, width, height);
    lv_obj_align(bar, align, 0, 0);
    lv_obj_remove_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    return bar;
}

static lv_obj_t *create_side_box(lv_obj_t *parent, int32_t width) {
    lv_obj_t *box = lv_container_create(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(box, width, LV_SIZE_CONTENT);
    return box;
}

static lv_obj_t *create_row(lv_obj_t *parent) {
    lv_obj_t *row = lv_container_create(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    return row;
}

void VideoPlayerScreen::build() {
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
}

VideoInsets VideoPlayerScreen::insets() const {
    VideoInsets insets;
    switch (mode_) {
    case UiMode::Bars:
        add_inset(insets, panel_edge(Edge::Top, rotation_), kTopBarHeight);
        add_inset(insets, panel_edge(Edge::Bottom, rotation_), bottom_height(rotation_));
        break;
    case UiMode::Settings:
    case UiMode::Info:
        if (is_portrait(rotation_)) {
            add_inset(insets, panel_edge(Edge::Bottom, rotation_), kPortraitPanelHeight);
        } else {
            add_inset(insets, panel_edge(Edge::Right, rotation_), kLandscapePanelWidth);
        }
        break;
    default:
        break;
    }
    return insets;
}

bool VideoPlayerScreen::openOverlay() {
    DisplayManagerConfig config = {};
    config.present_mode = DisplayPresentMode::Immediate;
    config.render_mode = DisplayRenderMode::Partial;
    config.color_format = LV_COLOR_FORMAT_RGB565;
    config.make_default = false;
    config.viewport.rotation = rotation_;
    config.buffer.lines = kOverlayBufferLines;
    if (display_manager.create_display(config, &ui_) != ESP_OK) {
        ui_ = nullptr;
        return false;
    }
    buildUi();
    return true;
}

void VideoPlayerScreen::closeOverlay() {
    if (!ui_) return;
    display_manager.delete_display(ui_);
    ui_ = nullptr;
    top_bar_ = nullptr;
    bottom_bar_ = nullptr;
    settings_ = nullptr;
    info_ = nullptr;
    info_button_ = nullptr;
    title_label_ = nullptr;
    play_label_ = nullptr;
    next_button_ = nullptr;
    repeat_label_ = nullptr;
    seek_ = nullptr;
    elapsed_label_ = nullptr;
    total_label_ = nullptr;
    volume_label_ = nullptr;
    volume_slider_ = nullptr;
    scrubbing_ = false;
    info_paused_ = false;
}

void VideoPlayerScreen::buildUi() {
    lv_obj_t *screen = lv_display_get_screen_active(ui_);
    lv_obj_clean(screen);
    lv_obj_set_style_bg_color(screen, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_white(), 0);
    lv_obj_set_style_pad_all(screen, 0, 0);
    lv_obj_remove_flag(screen, LV_OBJ_FLAG_SCROLLABLE);

    shown_elapsed_s_ = -2;
    shown_total_s_ = -2;
    shown_message_.clear();
    next_button_ = nullptr;

    /* Styleless, so a press changes nothing and never invalidates the video. */
    lv_obj_t *video = lv_container_create(screen);
    lv_obj_set_size(video, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(video, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_fn(video, LV_EVENT_CLICKED, [this](lv_event_t *) {
        requestMode(mode_ == UiMode::Hidden ? UiMode::Bars : UiMode::Hidden);
    });

    const bool portrait = is_portrait(rotation_);
    top_bar_ = create_bar(screen, lv_pct(100), kTopBarHeight, LV_ALIGN_TOP_MID);
    buildTopBar(top_bar_);
    bottom_bar_ = create_bar(screen, lv_pct(100), bottom_height(rotation_), LV_ALIGN_BOTTOM_MID);
    buildBottomBar(bottom_bar_, portrait);

    settings_ = portrait
        ? create_bar(screen, lv_pct(100), kPortraitPanelHeight, LV_ALIGN_BOTTOM_MID)
        : create_bar(screen, kLandscapePanelWidth, lv_pct(100), LV_ALIGN_RIGHT_MID);
    player_settings_panel_build(settings_, [this] { requestMode(UiMode::Bars); });

    info_ = portrait
        ? create_bar(screen, lv_pct(100), kPortraitPanelHeight, LV_ALIGN_BOTTOM_MID)
        : create_bar(screen, kLandscapePanelWidth, lv_pct(100), LV_ALIGN_RIGHT_MID);
    if (mode_ == UiMode::Info) populateInfo();

    lv_obj_set_flag(top_bar_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Bars);
    lv_obj_set_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Bars);
    lv_obj_set_flag(settings_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Settings);
    lv_obj_set_flag(info_, LV_OBJ_FLAG_HIDDEN, mode_ != UiMode::Info);

    /* A bar sits where it was created until the layout runs, and every area it
     * leaves on the way is painted with the screen behind it -- black, over the
     * video. Settle the layout here, where the invalidations can still be
     * dropped, so only the final areas are ever drawn. */
    lv_obj_update_layout(screen);
}

void VideoPlayerScreen::setMode(UiMode mode) {
    if (!ui_ || mode == mode_) return;
    mode_ = mode;

    if (mode == UiMode::Info) {
        info_paused_ = player_status().state == PlayerState::Playing;
        if (info_paused_) player_pause();
    }
    const bool resume = info_paused_ && mode != UiMode::Info;
    if (resume) info_paused_ = false;

    /* The UI leaving an area has to be painted out before the video takes it
     * back, and the video has to be clipped out of an area before the UI is
     * drawn into it. */
    if (mode != UiMode::Bars) {
        lv_obj_add_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    }
    if (mode != UiMode::Settings) lv_obj_add_flag(settings_, LV_OBJ_FLAG_HIDDEN);
    if (mode != UiMode::Info) lv_obj_add_flag(info_, LV_OBJ_FLAG_HIDDEN);
    lv_refr_now(ui_);
    bsp_display_wait_draw();
    video_presenter_set_ui_insets(insets());
    player_repaint();

    if (mode == UiMode::Bars) {
        media_volume_show(volume_label_, volume_slider_, settings_volume());
        lv_obj_remove_flag(top_bar_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(bottom_bar_, LV_OBJ_FLAG_HIDDEN);
    } else if (mode == UiMode::Settings) {
        lv_obj_send_event(settings_, LV_EVENT_REFRESH, nullptr);
        lv_obj_remove_flag(settings_, LV_OBJ_FLAG_HIDDEN);
    } else if (mode == UiMode::Info) {
        populateInfo();
        lv_obj_remove_flag(info_, LV_OBJ_FLAG_HIDDEN);
    }
    if (mode == UiMode::Hidden) return;
    lv_display_trigger_activity(ui_);
    refresh();

    /* After refresh(): the command is async, so the status it reads there still
     * says paused and the icon would flip back. */
    if (resume) {
        player_play();
        setPlayIcon(true);
    }
}

void VideoPlayerScreen::requestMode(UiMode mode) {
    lv_async_call([this, mode] {
        if (s_active == this) setMode(mode);
    });
}

void VideoPlayerScreen::rotate(bsp_rotation_t rotation) {
    if (rotation == rotation_) return;
    rotation_ = rotation;
    video_presenter_set_rotation(rotation);
    if (!ui_) return;

    /* The rotation resizes the screen, which invalidates all of it. Only the
     * bars are redrawn from here; the video clears and repaints its own area. */
    lv_refr_now(ui_);
    lv_display_enable_invalidation(ui_, false);
    display_manager.set_rotation(ui_, rotation);
    buildUi();
    lv_display_enable_invalidation(ui_, true);
    video_presenter_set_ui_insets(insets());
    player_repaint();
    if (mode_ == UiMode::Bars) {
        lv_obj_invalidate(top_bar_);
        lv_obj_invalidate(bottom_bar_);
    } else if (mode_ == UiMode::Settings) {
        lv_obj_invalidate(settings_);
    } else if (mode_ == UiMode::Info) {
        lv_obj_invalidate(info_);
    }
    refresh();
}

void VideoPlayerScreen::eject(const std::string &mount_point) {
    if (s_active && path_is_under(s_active->path(), mount_point)) s_active->back();
}

void VideoPlayerScreen::onEnter() {
    s_active = this;
    media_cache_stop();
    rotation_ = ui_orientation_current();
    mode_ = UiMode::Bars;
    info_paused_ = false;
    if (!openOverlay()) return;

    const SharedSram sram = media_player_acquire_sram();
    if (!video_presenter_begin(sram, rotation_)) {
        closeOverlay();
        media_player_release_sram();
        showStartError(video_presenter_error());
        return;
    }
    video_presenter_set_ui_insets(insets());
    ui_orientation_set_listener([](bsp_rotation_t rotation, void *arg) {
        static_cast<VideoPlayerScreen *>(arg)->rotate(rotation);
    }, this);

    /* The first LVGL pass covers the whole screen, video area included. Let it
     * land before any frame can: started the other way round the board shows
     * one frame, paints it out and only then plays. */
    lv_refr_now(ui_);
    bsp_display_wait_draw();

    player_observe_state(playerStateChanged);
    auto_start_tick_ = 0;
    openCurrent();
    timer_ = lv_timer_create([](lv_timer_t *timer) {
        static_cast<VideoPlayerScreen *>(lv_timer_get_user_data(timer))->tick();
    }, kRefreshPeriodMs, this);
}

void VideoPlayerScreen::onExit() {
    if (s_active == this) s_active = nullptr;
    player_observe_state(nullptr);
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (ui_) {
        player_close();
        closeOverlay();
        video_presenter_end();
        ui_orientation_set_listener(nullptr, nullptr);
        media_player_release_sram();
    }
    media_cache_start();
}

VideoPlayerScreen::~VideoPlayerScreen() {
    if (timer_) lv_timer_delete(timer_);
    closeOverlay();
}

void VideoPlayerScreen::showStartError(const std::string &message) {
    auto modal = lv_modal_open(root_);
    lv_modal_title_create(modal, "Video");
    lv_modal_message_create(modal, message.empty() ? "video output unavailable" : message.c_str());
    lv_modal_button_create(modal, "Close", LV_MODAL_BUTTON_TYPE_PRIMARY, [this, modal](lv_event_t *) {
        lv_modal_close(modal);
        back();
    });
}

void VideoPlayerScreen::populateInfo() {
    if (!info_) return;
    lv_obj_clean(info_);
    player_info_panel_build(info_, name(), player_media_summary(),
                            [this] { requestMode(UiMode::Bars); });
    lv_obj_update_layout(info_);
}

void VideoPlayerScreen::buildTopBar(lv_obj_t *parent) {
    const MediaTopBar bar = media_top_bar_build(parent, name().c_str(),
                                                [this] { this->back(); },
                                                [this] { requestMode(UiMode::Info); });
    title_label_ = bar.title;
    info_button_ = bar.info_button;
    lv_obj_set_state(info_button_, LV_STATE_DISABLED, !player_media_summary().valid);
}

void VideoPlayerScreen::buildBottomBar(lv_obj_t *parent, bool portrait) {
    lv_obj_set_style_pad_hor(parent, kBarPadding, 0);
    lv_obj_set_style_pad_ver(parent, 12, 0);

    if (portrait) {
        lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        buildSeekRow(parent);

        lv_obj_t *row = create_row(parent);
        lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
        lv_obj_set_style_pad_column(row, 0, 0);
        lv_spacer_create(row, kPortraitSide, 1);
        lv_obj_t *transport = create_row(row);
        lv_obj_set_size(transport, kPortraitSlider, LV_SIZE_CONTENT);
        lv_obj_set_flex_align(transport, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_column(transport, 16, 0);
        buildTransport(transport, false);
        buildTransport(create_side_box(row, kPortraitSide), true);

        buildVolumeRow(parent);
        return;
    }

    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(parent, 16, 0);
    buildTransport(parent, false);
    buildTransport(parent, true);

    lv_obj_t *right = lv_container_create(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_grow(right, 1);
    lv_obj_set_height(right, lv_pct(100));
    lv_obj_set_style_pad_left(right, 16, 0);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER);
    buildSeekRow(right);
    buildVolumeRow(right);
}

void VideoPlayerScreen::buildTransport(lv_obj_t *parent, bool repeat_only) {
    if (repeat_only) {
        lv_obj_t *repeat = media_icon_button(parent, kIconButton, &icon_36, TABLER_REPEAT_OFF,
                                             lv_color_white(), &repeat_label_);
        lv_obj_add_event_fn(repeat, LV_EVENT_CLICKED, [this](lv_event_t *) {
            switch (repeat_) {
            case RepeatMode::Off: setRepeatMode(RepeatMode::All); break;
            case RepeatMode::All: setRepeatMode(RepeatMode::One); break;
            case RepeatMode::One: setRepeatMode(RepeatMode::Off); break;
            }
            player_set_loop(repeat_ == RepeatMode::One);
        });
        setRepeatMode(repeat_);
        return;
    }

    lv_obj_t *prev = media_icon_button(parent, 96, &icon_48, TABLER_PLAYER_TRACK_PREV, lv_color_white());
    lv_obj_add_event_fn(prev, LV_EVENT_CLICKED, [this](lv_event_t *) {
        lv_display_trigger_activity(ui_);
        const PlayerStatus status = player_status();
        if (status.position_us < kPrevTrackUs && advance(-1, true)) return;
        restart(status.state == PlayerState::Playing || status.state == PlayerState::Finished);
    });

    lv_obj_t *play = media_icon_button(parent, 120, &icon_72, TABLER_PLAYER_PLAY, lv_color_white(),
                                           &play_label_);
    lv_obj_add_event_fn(play, LV_EVENT_CLICKED, [this](lv_event_t *) {
        if (playing_) {
            player_pause();
        } else {
            player_play();
        }
        setPlayIcon(!playing_);
        lv_display_trigger_activity(ui_);
    });

    next_button_ = media_icon_button(parent, 96, &icon_48, TABLER_PLAYER_TRACK_NEXT, lv_color_white());
    lv_obj_add_event_fn(next_button_, LV_EVENT_CLICKED, [this](lv_event_t *) {
        lv_display_trigger_activity(ui_);
        advance(1, true);
    });
    updateTransport();

    setPlayIcon(playing_);
}

void VideoPlayerScreen::openCurrent() {
    awaiting_start_ = true;
    scrubbing_ = false;
    stop_bars_shown_ = false;
    player_open(path());
    player_set_loop(repeat_ == RepeatMode::One);

    shown_elapsed_s_ = -2;
    shown_total_s_ = -2;
    setMessage({}, true);
    if (seek_) lv_slider_set_value(seek_, 0, LV_ANIM_OFF);
    updateTransport();
    if (ui_) lv_display_trigger_activity(ui_);
    refresh();
}

bool VideoPlayerScreen::advance(int delta, bool manual) {
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

void VideoPlayerScreen::restart(bool resume) {
    player_restart();
    if (!resume) return;
    player_play();
    setPlayIcon(true);
}

void VideoPlayerScreen::updateTransport() {
    if (!next_button_) return;
    lv_obj_set_state(next_button_, LV_STATE_DISABLED, !playlist_->canStep(1, repeat_));
}

void VideoPlayerScreen::playerStateChanged() {
    if (s_active) s_active->handleState();
}

void VideoPlayerScreen::handleState() {
    if (mode_ == UiMode::Info) return;

    const PlayerStatus status = player_status();
    if (awaiting_start_) {
        if (status.state == PlayerState::Paused) {
            awaiting_start_ = false;
            skips_ = 0;
            player_play();
            setPlayIcon(true);
            lv_display_trigger_activity(ui_);
            auto_start_tick_ = lv_tick_get();
            /* The status still says paused, so refreshing here would flip the
             * icon back. The Playing transition brings the next one. */
            return;
        }
        if (status.state == PlayerState::Failed) {
            awaiting_start_ = false;
            if (auto_opened_ && ++skips_ < (int)playlist_->size() && advance(1, false)) return;
        }
    } else if (status.state == PlayerState::Finished && advance(1, false)) {
        return;
    }

    const bool stopped = !awaiting_start_ && (status.state == PlayerState::Finished ||
                                              status.state == PlayerState::Failed);
    if (!stopped) {
        stop_bars_shown_ = false;
    } else if (!stop_bars_shown_) {
        stop_bars_shown_ = true;
        if (mode_ == UiMode::Hidden) setMode(UiMode::Bars);
    }
    refresh();
}

void VideoPlayerScreen::buildSeekRow(lv_obj_t *parent) {
    const bool portrait = is_portrait(rotation_);
    lv_obj_t *row = create_row(parent);
    lv_obj_set_size(row, lv_pct(100), 56);
    lv_obj_set_style_pad_column(row, portrait ? 0 : kSeekGap, 0);

    elapsed_label_ = lv_label_create(row);
    lv_obj_set_width(elapsed_label_, portrait ? kPortraitSide : kTimeWidth);
    lv_obj_set_style_text_align(elapsed_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_font_role(elapsed_label_, LV_WIDGETS_FONT_BODY);

    seek_ = media_slider(row, kSeekRange, lv_color_white(), lv_color_hex(kTrackColor));
    if (portrait) {
        lv_obj_set_width(seek_, kPortraitSlider);
    } else {
        lv_obj_set_flex_grow(seek_, 1);
    }
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
    lv_obj_set_width(total_label_, portrait ? kPortraitSide : kTimeWidth);
    lv_obj_set_style_text_align(total_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_font_role(total_label_, LV_WIDGETS_FONT_BODY);
}

void VideoPlayerScreen::buildVolumeRow(lv_obj_t *parent) {
    const bool portrait = is_portrait(rotation_);
    lv_obj_t *row = create_row(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const int32_t side = portrait ? kPortraitSide : kTimeWidth;
    lv_obj_set_style_pad_column(row, portrait ? 0 : kSeekGap, 0);

    lv_obj_t *mute = media_icon_button(create_side_box(row, side), kIconButton, &icon_36,
                                       TABLER_VOLUME, lv_color_white(), &volume_label_);

    volume_slider_ = media_slider(row, 100, lv_color_white(), lv_color_hex(kTrackColor));
    lv_obj_set_width(volume_slider_, portrait ? kPortraitSlider : kVolumeSlider);
    media_volume_bind(mute, volume_label_, volume_slider_);

    lv_obj_t *settings = media_icon_button(create_side_box(row, side), kIconButton, &icon_36,
                                           TABLER_ADJUSTMENTS_HORIZONTAL, lv_color_white());
    lv_obj_add_event_fn(settings, LV_EVENT_CLICKED, [this](lv_event_t *) {
        requestMode(UiMode::Settings);
    });
}

void VideoPlayerScreen::setRepeatMode(RepeatMode mode) {
    repeat_ = mode;
    updateTransport();
    if (!repeat_label_) return;
    switch (mode) {
    case RepeatMode::Off: lv_label_set_text(repeat_label_, TABLER_REPEAT_OFF); break;
    case RepeatMode::All: lv_label_set_text(repeat_label_, TABLER_REPEAT); break;
    case RepeatMode::One: lv_label_set_text(repeat_label_, TABLER_REPEAT_ONCE); break;
    }
}

void VideoPlayerScreen::setPlayIcon(bool playing) {
    playing_ = playing;
    if (play_label_) lv_label_set_text(play_label_, playing ? TABLER_PLAYER_PAUSE : TABLER_PLAYER_PLAY);
}

void VideoPlayerScreen::setTime(lv_obj_t *label, int64_t *shown_s, int64_t us) {
    const int64_t seconds = us < 0 ? -1 : us / 1000000;
    if (seconds == *shown_s) return;
    *shown_s = seconds;
    char text[16];
    media_format_time(text, sizeof(text), seconds);
    lv_label_set_text(label, text);
}

void VideoPlayerScreen::tick() {
    handleState();

    if (mode_ != UiMode::Bars) return;
    if (!playing_ || scrubbing_) return;
    if (volume_slider_ && lv_obj_has_state(volume_slider_, LV_STATE_PRESSED)) return;
    const uint32_t inactive_ms = lv_display_get_inactive_time(nullptr);
    const bool untouched = auto_start_tick_ && inactive_ms >= lv_tick_elaps(auto_start_tick_);
    if (inactive_ms < (untouched ? kAutoStartHideMs : kAutoHideMs)) return;
    setMode(UiMode::Hidden);
}

void VideoPlayerScreen::setMessage(const std::string &message, bool force) {
    if (!title_label_ || (!force && message == shown_message_)) return;
    shown_message_ = message;
    lv_label_set_text(title_label_, message.empty() ? name().c_str() : message.c_str());
    lv_obj_set_style_text_color(title_label_, message.empty() ? lv_color_white()
                                                              : lv_color_hex(kMessageColor), 0);
}

void VideoPlayerScreen::refresh() {
    if (!seek_ || mode_ != UiMode::Bars) return;

    /* Until the file the player was told to open is loaded, its status and
     * summary still describe the previous one. */
    const bool ready = !awaiting_start_;
    const PlayerStatus status = player_status();
    const bool playing = ready && status.state == PlayerState::Playing;
    if (playing != playing_) setPlayIcon(playing);
    const MediaSummary summary = ready ? player_media_summary() : MediaSummary{};
    lv_obj_set_state(info_button_, LV_STATE_DISABLED, !summary.valid);

    const bool known = ready && status.duration_us > 0 && status.state != PlayerState::Loading;
    setTime(total_label_, &shown_total_s_, known ? status.duration_us : -1);
    if (!scrubbing_) {
        setTime(elapsed_label_, &shown_elapsed_s_, known ? status.position_us : 0);
        const int64_t value = known ? status.position_us * kSeekRange / status.duration_us : 0;
        lv_slider_set_value(seek_, (int32_t)value, LV_ANIM_OFF);
    }

    std::string message;
    if (ready) {
        if (status.state == PlayerState::Failed) message = status.error;
        if (message.empty()) message = video_presenter_error();
        if (message.empty() && summary.valid && summary.video.codec == CodecId::None) {
            message = "no video track";
        }
        if (message.empty()) message = status.audio_note;
    }
    setMessage(message, false);
}
