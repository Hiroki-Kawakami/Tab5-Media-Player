/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "player_screen.hpp"
#include "audio/audio_out.hpp"
#include "media_player.hpp"
#include "playback/player.hpp"
#include "video/video_presenter.hpp"
#include "ui_orientation.hpp"
#include "bsp.h"
#include "display_manager.hpp"
#include "resources.h"

#include <cstdio>
#include <cstring>

static constexpr int32_t kTopBarHeight = 80;
static constexpr int32_t kLandscapeBottomHeight = 160;
static constexpr int32_t kPortraitBottomHeight = 272;
static constexpr uint32_t kRefreshPeriodMs = 500;
static constexpr uint32_t kAutoStartPollMs = 100;
static constexpr uint32_t kAutoHideMs = 4000;
static constexpr uint32_t kAutoStartHideMs = 1500;
static constexpr int32_t kSeekRange = 1000;
static constexpr int32_t kBarPadding = 24;
static constexpr int32_t kPortraitSide = 116;
static constexpr int32_t kPortraitSlider = 440;
static constexpr int32_t kTimeWidth = 100;
static constexpr int32_t kIconButton = 72;
static constexpr int32_t kVolumeSlider = 240;
static constexpr int32_t kSeekGap = 16;
static constexpr int32_t kTopBarPadding = 8;

static constexpr uint32_t kBarColor = 0x101010;
static constexpr uint32_t kMessageColor = 0xffb74d;

enum class Edge { Top, Bottom, Left, Right };

static lv_display_t *s_top;
static lv_display_t *s_bottom;
static bool s_bar_visible;
static bool s_outside_down;
static PlayerScreen *s_active;

static bool is_portrait(bsp_rotation_t rotation) {
    return rotation == BSP_ROTATION_0 || rotation == BSP_ROTATION_180;
}

static int32_t bottom_height(bsp_rotation_t rotation) {
    return is_portrait(rotation) ? kPortraitBottomHeight : kLandscapeBottomHeight;
}

static Edge panel_edge(bsp_rotation_t rotation, bool bottom) {
    switch (rotation) {
    case BSP_ROTATION_90:  return bottom ? Edge::Right : Edge::Left;
    case BSP_ROTATION_180: return bottom ? Edge::Top : Edge::Bottom;
    case BSP_ROTATION_270: return bottom ? Edge::Left : Edge::Right;
    default:               return bottom ? Edge::Bottom : Edge::Top;
    }
}

static bsp_rect_t edge_area(Edge edge, int32_t thickness, bsp_size_t panel) {
    switch (edge) {
    case Edge::Top:    return { { 0, 0 }, { panel.width, thickness } };
    case Edge::Bottom: return { { 0, panel.height - thickness }, { panel.width, thickness } };
    case Edge::Left:   return { { 0, 0 }, { thickness, panel.height } };
    default:           return { { panel.width - thickness, 0 }, { thickness, panel.height } };
    }
}

static void add_inset(VideoInsets &insets, Edge edge, int32_t thickness) {
    switch (edge) {
    case Edge::Top:    insets.top = thickness; break;
    case Edge::Bottom: insets.bottom = thickness; break;
    case Edge::Left:   insets.left = thickness; break;
    default:           insets.right = thickness; break;
    }
}

static VideoInsets bar_insets(bsp_rotation_t rotation) {
    VideoInsets insets;
    add_inset(insets, panel_edge(rotation, false), kTopBarHeight);
    add_inset(insets, panel_edge(rotation, true), bottom_height(rotation));
    return insets;
}

static void show_bars(bsp_rotation_t rotation) {
    video_presenter_set_ui_insets(bar_insets(rotation));
    display_manager.set_visible(s_top, true);
    display_manager.set_visible(s_bottom, true);
}

static void hide_bars() {
    display_manager.set_visible(s_top, false);
    display_manager.set_visible(s_bottom, false);
    bsp_display_wait_draw();
    video_presenter_set_ui_insets({});
}

static void set_bar_visible(bool visible) {
    if (!s_top || !s_bottom || !s_active || visible == s_bar_visible) return;
    s_bar_visible = visible;
    if (visible) {
        show_bars(s_active->rotation());
        lv_display_trigger_activity(s_bottom);
        s_active->refresh();
    } else {
        hide_bars();
    }
}

static void outside_touch(const bsp_touch_point_t *, int count, void *) {
    if (count <= 0) {
        s_outside_down = false;
        return;
    }
    if (s_outside_down) return;
    s_outside_down = true;

    lv_lock();
    lv_async_call([] { set_bar_visible(!s_bar_visible); });
    lv_unlock();
}

static void format_time(char *text, size_t size, int64_t seconds) {
    if (seconds < 0) {
        snprintf(text, size, "--:--");
    } else if (seconds >= 3600) {
        snprintf(text, size, "%d:%02d:%02d", (int)(seconds / 3600), (int)(seconds / 60 % 60),
                 (int)(seconds % 60));
    } else {
        snprintf(text, size, "%d:%02d", (int)(seconds / 60), (int)(seconds % 60));
    }
}

static lv_obj_t *create_bar_root(lv_display_t *display) {
    lv_obj_t *root = lv_display_get_screen_active(display);
    lv_obj_set_style_bg_color(root, lv_color_hex(kBarColor), 0);
    lv_obj_set_style_bg_opa(root, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(root, lv_color_white(), 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    return root;
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

static lv_obj_t *create_icon_button(lv_obj_t *parent, int32_t size, const lv_font_t *font,
                                    const char *icon, lv_obj_t **label = nullptr) {
    lv_obj_t *button = lv_button_create(parent, LV_BUTTON_STYLE_PLAIN);
    lv_obj_set_size(button, size, size);
    lv_obj_set_style_pad_all(button, 0, 0);
    lv_obj_set_style_radius(button, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(button, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(button, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_style_text_color(button, lv_color_white(), 0);
    lv_obj_set_style_text_color(button, lv_color_hex(0x606060), LV_STATE_DISABLED);
    lv_obj_t *icon_label = lv_button_set_text(button, icon, font);
    if (label) *label = icon_label;
    return button;
}

static lv_obj_t *create_slider(lv_obj_t *parent, int32_t max) {
    lv_obj_t *slider = lv_slider_create(parent);
    lv_slider_set_range(slider, 0, max);
    lv_obj_set_height(slider, 8);
    lv_obj_set_style_bg_color(slider, lv_color_hex(0x404040), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 8, LV_PART_KNOB);
    return slider;
}

void PlayerScreen::build() {
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
}

bool PlayerScreen::openOverlay() {
    const bsp_size_t panel = bsp_display_get_size();
    DisplayManagerConfig config = {};
    config.present_mode = DisplayPresentMode::Immediate;
    config.render_mode = DisplayRenderMode::Partial;
    config.make_default = false;
    config.visible = false;
    config.viewport.rotation = rotation_;

    config.viewport.output_area = edge_area(panel_edge(rotation_, false), kTopBarHeight, panel);
    if (display_manager.create_display(config, &top_) != ESP_OK) {
        top_ = nullptr;
        return false;
    }
    config.viewport.output_area = edge_area(panel_edge(rotation_, true), bottom_height(rotation_), panel);
    if (display_manager.create_display(config, &bottom_) != ESP_OK) {
        display_manager.delete_display(top_);
        top_ = nullptr;
        bottom_ = nullptr;
        return false;
    }

    shown_elapsed_s_ = -2;
    shown_total_s_ = -2;
    shown_message_.clear();
    buildTopBar(create_bar_root(top_));
    buildBottomBar(create_bar_root(bottom_), is_portrait(rotation_));
    s_top = top_;
    s_bottom = bottom_;
    refresh();
    return true;
}

void PlayerScreen::closeOverlay() {
    if (!top_) return;
    s_top = nullptr;
    s_bottom = nullptr;
    display_manager.delete_display(bottom_);
    display_manager.delete_display(top_);
    top_ = nullptr;
    bottom_ = nullptr;
    title_label_ = nullptr;
    play_label_ = nullptr;
    repeat_label_ = nullptr;
    seek_ = nullptr;
    elapsed_label_ = nullptr;
    total_label_ = nullptr;
    volume_label_ = nullptr;
    volume_slider_ = nullptr;
    scrubbing_ = false;
}

void PlayerScreen::rotate(bsp_rotation_t rotation) {
    if (rotation == rotation_) return;
    closeOverlay();
    rotation_ = rotation;
    video_presenter_set_rotation(rotation);
    if (!openOverlay()) {
        video_presenter_set_ui_insets({});
    } else if (s_bar_visible) {
        show_bars(rotation_);
    }
}

void PlayerScreen::eject(const std::string &mount_point) {
    if (s_active && path_is_under(s_active->path_, mount_point)) s_active->back();
}

void PlayerScreen::onEnter() {
    s_active = this;
    rotation_ = ui_orientation_current();
    s_bar_visible = true;
    s_outside_down = false;
    if (!openOverlay()) return;

    const SharedSram sram = media_player_acquire_sram();
    if (!video_presenter_begin(sram, rotation_)) {
        closeOverlay();
        media_player_release_sram();
        showStartError(video_presenter_error());
        return;
    }
    show_bars(rotation_);
    ui_orientation_set_listener([](bsp_rotation_t rotation, void *arg) {
        static_cast<PlayerScreen *>(arg)->rotate(rotation);
    }, this);
    display_manager.set_outside_touch_callback(outside_touch);
    player_open(path_);
    player_set_loop(repeat_ != RepeatMode::Off);
    lv_display_trigger_activity(bottom_);

    auto_start_ = true;
    auto_start_tick_ = 0;
    timer_ = lv_timer_create([](lv_timer_t *timer) {
        static_cast<PlayerScreen *>(lv_timer_get_user_data(timer))->tick();
    }, kAutoStartPollMs, this);
    refresh();
}

void PlayerScreen::onExit() {
    if (s_active == this) s_active = nullptr;
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (!top_) return;
    display_manager.set_outside_touch_callback(nullptr);
    player_close();
    closeOverlay();
    video_presenter_end();
    ui_orientation_set_listener(nullptr, nullptr);
    media_player_release_sram();
}

PlayerScreen::~PlayerScreen() {
    if (timer_) lv_timer_delete(timer_);
    closeOverlay();
}

void PlayerScreen::showStartError(const std::string &message) {
    auto modal = lv_modal_open(root_);
    lv_modal_title_create(modal, "Video");
    lv_modal_message_create(modal, message.empty() ? "video output unavailable" : message.c_str());
    lv_modal_button_create(modal, "Close", LV_MODAL_BUTTON_TYPE_PRIMARY, [this, modal](lv_event_t *) {
        lv_modal_close(modal);
        back();
    });
}

void PlayerScreen::buildTopBar(lv_obj_t *parent) {
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_hor(parent, kTopBarPadding, 0);
    lv_obj_set_style_pad_column(parent, kTopBarPadding, 0);

    lv_obj_t *back = lv_button_create(parent, LV_BUTTON_STYLE_PLAIN);
    lv_obj_set_size(back, LV_SIZE_CONTENT, kIconButton);
    lv_obj_set_style_pad_all(back, 8, 0);
    lv_obj_set_style_pad_column(back, 16, 0);
    lv_obj_set_style_radius(back, 16, 0);
    lv_obj_set_style_bg_color(back, lv_color_white(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(back, LV_OPA_20, LV_STATE_PRESSED);
    lv_obj_set_flex_flow(back, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(back, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_fn(back, LV_EVENT_CLICKED, [this](lv_event_t *) { this->back(); });

    lv_obj_t *chevron = lv_label_create(back);
    lv_obj_set_style_pad_all(chevron, 8, 0);
    lv_obj_set_style_text_font(chevron, &icon_36, 0);
    lv_label_set_text(chevron, TABLER_CHEVRON_LEFT);

    title_label_ = lv_label_create(back);
    lv_obj_set_height(title_label_, lv_font_get_line_height(lv_widgets_body_font()) + 16);
    lv_obj_set_style_pad_right(title_label_, 24, 0);
    lv_obj_set_style_pad_ver(title_label_, 8, 0);
    lv_obj_set_style_text_font(title_label_, lv_widgets_body_font(), 0);
    lv_label_set_text(title_label_, name_.c_str());

    lv_spacer_create(parent, 1, 1, 1);

    lv_obj_t *info = create_icon_button(parent, kIconButton, &icon_36, TABLER_INFO_CIRCLE);
    lv_obj_add_state(info, LV_STATE_DISABLED);

    lv_obj_update_layout(parent);
    const int32_t room = lv_obj_get_width(parent) - 2 * kTopBarPadding - kTopBarPadding -
                         kIconButton - (lv_obj_get_width(back) - lv_obj_get_width(title_label_));
    if (lv_obj_get_width(title_label_) > room) {
        lv_obj_set_width(title_label_, room);
        lv_label_set_long_mode(title_label_, LV_LABEL_LONG_MODE_DOTS);
    }
}

void PlayerScreen::buildBottomBar(lv_obj_t *parent, bool portrait) {
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

void PlayerScreen::buildTransport(lv_obj_t *parent, bool repeat_only) {
    if (repeat_only) {
        lv_obj_t *repeat = create_icon_button(parent, kIconButton, &icon_36, TABLER_REPEAT_OFF,
                                              &repeat_label_);
        lv_obj_add_event_fn(repeat, LV_EVENT_CLICKED, [this](lv_event_t *) {
            switch (repeat_) {
            case RepeatMode::Off: setRepeatMode(RepeatMode::All); break;
            case RepeatMode::All: setRepeatMode(RepeatMode::One); break;
            case RepeatMode::One: setRepeatMode(RepeatMode::Off); break;
            }
            player_set_loop(repeat_ != RepeatMode::Off);
        });
        setRepeatMode(repeat_);
        return;
    }

    lv_obj_t *prev = create_icon_button(parent, 96, &icon_48, TABLER_PLAYER_TRACK_PREV);
    lv_obj_add_event_fn(prev, LV_EVENT_CLICKED, [](lv_event_t *) { player_restart(); });

    lv_obj_t *play = create_icon_button(parent, 120, &icon_72, TABLER_PLAYER_PLAY, &play_label_);
    lv_obj_add_event_fn(play, LV_EVENT_CLICKED, [this](lv_event_t *) {
        if (playing_) {
            player_pause();
        } else {
            player_play();
        }
        setPlayIcon(!playing_);
        lv_display_trigger_activity(bottom_);
    });

    lv_obj_t *next = create_icon_button(parent, 96, &icon_48, TABLER_PLAYER_TRACK_NEXT);
    lv_obj_add_state(next, LV_STATE_DISABLED);

    setPlayIcon(playing_);
}

void PlayerScreen::buildSeekRow(lv_obj_t *parent) {
    const bool portrait = is_portrait(rotation_);
    lv_obj_t *row = create_row(parent);
    lv_obj_set_size(row, lv_pct(100), 56);
    lv_obj_set_style_pad_column(row, portrait ? 0 : kSeekGap, 0);

    elapsed_label_ = lv_label_create(row);
    lv_obj_set_width(elapsed_label_, portrait ? kPortraitSide : kTimeWidth);
    lv_obj_set_style_text_align(elapsed_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(elapsed_label_, lv_widgets_body_font(), 0);

    seek_ = create_slider(row, kSeekRange);
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
    lv_obj_set_style_text_font(total_label_, lv_widgets_body_font(), 0);
}

void PlayerScreen::buildVolumeRow(lv_obj_t *parent) {
    const bool portrait = is_portrait(rotation_);
    lv_obj_t *row = create_row(parent);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const int32_t side = portrait ? kPortraitSide : kTimeWidth;
    lv_obj_set_style_pad_column(row, portrait ? 0 : kSeekGap, 0);

    lv_obj_t *mute = create_icon_button(create_side_box(row, side), kIconButton,
                                        &icon_36, TABLER_VOLUME, &volume_label_);
    lv_obj_add_event_fn(mute, LV_EVENT_CLICKED, [this](lv_event_t *) {
        muted_ = !muted_;
        audio_out_set_volume(muted_ ? 0 : lv_slider_get_value(volume_slider_));
        setVolumeIcon(lv_slider_get_value(volume_slider_));
    });

    volume_slider_ = create_slider(row, 100);
    lv_obj_set_width(volume_slider_, portrait ? kPortraitSlider : kVolumeSlider);
    lv_slider_set_value(volume_slider_, audio_out_get_volume(), LV_ANIM_OFF);
    setVolumeIcon(audio_out_get_volume());
    lv_obj_add_event_fn(volume_slider_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t *) {
        const int32_t volume = lv_slider_get_value(volume_slider_);
        muted_ = false;
        audio_out_set_volume(volume);
        setVolumeIcon(volume);
    });

    lv_obj_t *settings = create_icon_button(create_side_box(row, side), kIconButton,
                                            &icon_36, TABLER_ADJUSTMENTS_HORIZONTAL);
    lv_obj_add_state(settings, LV_STATE_DISABLED);
}

void PlayerScreen::setRepeatMode(RepeatMode mode) {
    repeat_ = mode;
    if (!repeat_label_) return;
    switch (mode) {
    case RepeatMode::Off: lv_label_set_text(repeat_label_, TABLER_REPEAT_OFF); break;
    case RepeatMode::All: lv_label_set_text(repeat_label_, TABLER_REPEAT); break;
    case RepeatMode::One: lv_label_set_text(repeat_label_, TABLER_REPEAT_ONCE); break;
    }
}

void PlayerScreen::setPlayIcon(bool playing) {
    playing_ = playing;
    if (play_label_) lv_label_set_text(play_label_, playing ? TABLER_PLAYER_PAUSE : TABLER_PLAYER_PLAY);
}

void PlayerScreen::setVolumeIcon(int32_t volume) {
    const char *icon = muted_       ? TABLER_VOLUME_3
                     : volume <= 0  ? TABLER_VOLUME_4
                     : volume < 50  ? TABLER_VOLUME_2
                                    : TABLER_VOLUME;
    if (strcmp(lv_label_get_text(volume_label_), icon) != 0) lv_label_set_text(volume_label_, icon);
}

void PlayerScreen::setTime(lv_obj_t *label, int64_t *shown_s, int64_t us) {
    const int64_t seconds = us < 0 ? -1 : us / 1000000;
    if (seconds == *shown_s) return;
    *shown_s = seconds;
    char text[16];
    format_time(text, sizeof(text), seconds);
    lv_label_set_text(label, text);
}

void PlayerScreen::tick() {
    const PlayerState state = player_status().state;
    if (auto_start_ && (state == PlayerState::Paused || state == PlayerState::Failed)) {
        auto_start_ = false;
        lv_timer_set_period(timer_, kRefreshPeriodMs);
        if (state == PlayerState::Paused) {
            player_play();
            setPlayIcon(true);
            lv_display_trigger_activity(bottom_);
            auto_start_tick_ = lv_tick_get();
        }
    }

    const bool stopped = state == PlayerState::Finished || state == PlayerState::Failed;
    if (!stopped) {
        stop_bars_shown_ = false;
    } else if (!stop_bars_shown_) {
        stop_bars_shown_ = true;
        set_bar_visible(true);
    }

    if (!s_bar_visible) return;
    refresh();
    if (!playing_ || scrubbing_) return;
    if (volume_slider_ && lv_obj_has_state(volume_slider_, LV_STATE_PRESSED)) return;
    const uint32_t inactive_ms = lv_display_get_inactive_time(nullptr);
    const bool untouched = auto_start_tick_ && inactive_ms >= lv_tick_elaps(auto_start_tick_);
    if (inactive_ms < (untouched ? kAutoStartHideMs : kAutoHideMs)) return;
    set_bar_visible(false);
}

void PlayerScreen::refresh() {
    if (!seek_ || !s_bar_visible) return;

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
    if (message.empty()) message = video_presenter_error();
    if (message.empty()) message = status.audio_note;
    if (message != shown_message_) {
        shown_message_ = message;
        lv_label_set_text(title_label_, message.empty() ? name_.c_str() : message.c_str());
        lv_obj_set_style_text_color(title_label_, message.empty() ? lv_color_white()
                                                                  : lv_color_hex(kMessageColor), 0);
    }
}
