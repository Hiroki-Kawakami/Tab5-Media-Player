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

#include <cstdio>

static constexpr int32_t kLandscapeBarHeight = 160;
static constexpr int32_t kPortraitBarHeight = 240;
static constexpr uint32_t kRefreshPeriodMs = 300;

static lv_display_t *s_bar;
static bool s_bar_visible;
static bool s_outside_down;

static bool is_portrait(bsp_rotation_t rotation) {
    return rotation == BSP_ROTATION_0 || rotation == BSP_ROTATION_180;
}

static bsp_rect_t bar_area(bsp_rotation_t rotation, bsp_size_t panel) {
    const int32_t height = is_portrait(rotation) ? kPortraitBarHeight : kLandscapeBarHeight;
    switch (rotation) {
    case BSP_ROTATION_90:  return { { panel.width - height, 0 }, { height, panel.height } };
    case BSP_ROTATION_180: return { { 0, 0 }, { panel.width, height } };
    case BSP_ROTATION_270: return { { 0, 0 }, { height, panel.height } };
    default:               return { { 0, panel.height - height }, { panel.width, height } };
    }
}

static void set_bar_visible(bool visible) {
    if (!s_bar || visible == s_bar_visible) return;
    s_bar_visible = visible;
    display_manager.set_visible(s_bar, visible);
    if (!visible) video_presenter_repaint();
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

void PlayerScreen::build() {
    lv_obj_set_style_bg_color(root_, lv_color_black(), 0);
}

bool PlayerScreen::openOverlay() {
    DisplayManagerConfig config = {};
    config.present_mode = DisplayPresentMode::Deferred;
    config.make_default = false;
    config.viewport.rotation = rotation_;
    config.viewport.output_area = bar_area(rotation_, bsp_display_get_size());
    if (display_manager.create_display(config, &overlay_) != ESP_OK) {
        overlay_ = nullptr;
        return false;
    }
    lv_display_add_event_cb(
        overlay_, [](lv_event_t *) { video_presenter_mark_overlay_dirty(); },
        LV_EVENT_RENDER_READY, nullptr);

    buildOverlay(lv_display_get_screen_active(overlay_), is_portrait(rotation_));
    display_manager.set_visible(overlay_, s_bar_visible);
    s_bar = overlay_;
    refresh();
    return true;
}

void PlayerScreen::closeOverlay() {
    if (!overlay_) return;
    s_bar = nullptr;
    display_manager.delete_display(overlay_);
    overlay_ = nullptr;
    play_label_ = nullptr;
    loop_label_ = nullptr;
    progress_ = nullptr;
    time_label_ = nullptr;
    status_label_ = nullptr;
    volume_slider_ = nullptr;
    scrubbing_ = false;
}

void PlayerScreen::rotate(bsp_rotation_t rotation) {
    if (rotation == rotation_) return;
    video_presenter_set_overlay(nullptr);
    closeOverlay();
    rotation_ = rotation;
    video_presenter_set_rotation(rotation);
    if (openOverlay()) video_presenter_set_overlay(overlay_);
}

void PlayerScreen::onEnter() {
    rotation_ = ui_orientation_current();
    s_bar_visible = true;
    s_outside_down = false;
    if (!openOverlay()) return;

    const SharedSram sram = media_player_acquire_sram();
    video_presenter_begin(sram, rotation_);
    video_presenter_set_overlay(overlay_);
    ui_orientation_set_listener([](bsp_rotation_t rotation, void *arg) {
        static_cast<PlayerScreen *>(arg)->rotate(rotation);
    }, this);
    display_manager.set_outside_touch_callback(outside_touch);
    player_open(path_);

    timer_ = lv_timer_create([](lv_timer_t *timer) {
        static_cast<PlayerScreen *>(lv_timer_get_user_data(timer))->refresh();
    }, kRefreshPeriodMs, this);
    refresh();
}

void PlayerScreen::onExit() {
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (!overlay_) return;
    display_manager.set_outside_touch_callback(nullptr);
    player_close();
    video_presenter_end();
    video_presenter_set_overlay(nullptr);
    closeOverlay();
    ui_orientation_set_listener(nullptr, nullptr);
    media_player_release_sram();
}

PlayerScreen::~PlayerScreen() {
    if (timer_) lv_timer_delete(timer_);
    closeOverlay();
}

void PlayerScreen::buildOverlay(lv_obj_t *parent, bool portrait) {
    lv_obj_set_style_bg_color(parent, lv_color_hex(0x101010), 0);
    lv_obj_set_style_bg_opa(parent, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_hor(parent, 20, 0);
    lv_obj_set_style_pad_ver(parent, 10, 0);
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(parent, 8, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *row = lv_container_create(parent, portrait ? LV_FLEX_FLOW_ROW_WRAP : LV_FLEX_FLOW_ROW);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, 0);
    lv_obj_set_style_pad_row(row, 8, 0);

    lv_obj_t *back = lv_button_create(row, LV_BUTTON_STYLE_PLAIN);
    lv_obj_set_size(back, 90, 72);
    lv_obj_t *back_label = lv_button_set_text(back, LV_SYMBOL_LEFT, lv_widgets_body_font());
    lv_obj_set_style_text_color(back_label, lv_color_white(), 0);
    lv_obj_add_event_fn(back, LV_EVENT_CLICKED, [this](lv_event_t *) { this->back(); });

    lv_obj_t *play = lv_button_create(row, LV_BUTTON_STYLE_PRIMARY);
    lv_obj_set_size(play, 190, 72);
    play_label_ = lv_button_set_text(play, LV_SYMBOL_PLAY "  Play", lv_widgets_body_font());
    lv_obj_add_event_fn(play, LV_EVENT_CLICKED, [this](lv_event_t *) {
        if (playing_) {
            player_pause();
        } else {
            player_play();
        }
    });

    lv_obj_t *restart = lv_button_create(row, LV_BUTTON_STYLE_SECONDARY);
    lv_obj_set_size(restart, 110, 72);
    lv_button_set_text(restart, LV_SYMBOL_REFRESH, lv_widgets_body_font());
    lv_obj_add_event_fn(restart, LV_EVENT_CLICKED, [](lv_event_t *) { player_restart(); });

    lv_obj_t *loop = lv_button_create(row, LV_BUTTON_STYLE_SECONDARY);
    lv_obj_set_size(loop, 110, 72);
    loop_label_ = lv_button_set_text(loop, LV_SYMBOL_LOOP, lv_widgets_body_font());
    lv_obj_add_event_fn(loop, LV_EVENT_CLICKED, [this](lv_event_t *) {
        looping_ = !looping_;
        player_set_loop(looping_);
        setLoopIndicator(looping_);
    });
    looping_ = false;
    setLoopIndicator(looping_);

    time_label_ = lv_label_create(row);
    lv_obj_set_width(time_label_, 330);
    lv_obj_set_style_text_font(time_label_, lv_widgets_body_font(), 0);
    lv_obj_set_style_text_color(time_label_, lv_color_white(), 0);
    lv_label_set_text(time_label_, "");

    lv_obj_t *volume_icon = lv_label_create(row);
    lv_label_set_text(volume_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_font(volume_icon, lv_widgets_body_font(), 0);
    lv_obj_set_style_text_color(volume_icon, lv_color_white(), 0);

    volume_slider_ = lv_slider_create(row);
    lv_obj_set_width(volume_slider_, 200);
    lv_slider_set_range(volume_slider_, 0, 100);
    lv_slider_set_value(volume_slider_, audio_out_get_volume(), LV_ANIM_OFF);
    lv_obj_add_event_fn(volume_slider_, LV_EVENT_VALUE_CHANGED, [this](lv_event_t *) {
        audio_out_set_volume(lv_slider_get_value(volume_slider_));
    });

    progress_ = lv_slider_create(parent);
    lv_obj_set_width(progress_, lv_pct(100));
    lv_slider_set_range(progress_, 0, 1000);
    lv_slider_set_value(progress_, 0, LV_ANIM_OFF);
    lv_obj_add_event_fn(progress_, LV_EVENT_PRESSED, [this](lv_event_t *) { scrubbing_ = true; });
    lv_obj_add_event_fn(progress_, LV_EVENT_RELEASED, [this](lv_event_t *) {
        scrubbing_ = false;
        const PlayerStatus status = player_status();
        if (!status.seekable || status.duration_us <= 0) return;
        player_seek((int64_t)lv_slider_get_value(progress_) * status.duration_us / 1000);
    });

    status_label_ = lv_label_create(parent);
    lv_obj_set_width(status_label_, lv_pct(100));
    lv_label_set_long_mode(status_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_font(status_label_, lv_widgets_body_font(), 0);
    lv_obj_set_style_text_color(status_label_, lv_palette_main(LV_PALETTE_GREY), 0);
    lv_label_set_text(status_label_, "");
}

void PlayerScreen::setLoopIndicator(bool on) {
    if (!loop_label_) return;
    lv_obj_set_style_text_color(loop_label_,
                                on ? lv_palette_main(LV_PALETTE_BLUE)
                                   : lv_palette_main(LV_PALETTE_GREY), 0);
    lv_obj_set_style_text_opa(loop_label_, on ? LV_OPA_COVER : LV_OPA_60, 0);
}

void PlayerScreen::refresh() {
    if (!status_label_ || !s_bar_visible) return;

    const PlayerStatus status = player_status();
    playing_ = status.state == PlayerState::Playing;
    lv_label_set_text(play_label_, playing_ ? LV_SYMBOL_PAUSE "  Pause" : LV_SYMBOL_PLAY "  Play");

    if (status.loop != looping_) {
        looping_ = status.loop;
        setLoopIndicator(looping_);
    }

    if (!scrubbing_) {
        const int32_t value = status.duration_us > 0
            ? (int32_t)(status.position_us * 1000 / status.duration_us) : 0;
        lv_slider_set_value(progress_, value, LV_ANIM_OFF);
    }

    char text[160];
    if (status.state == PlayerState::Failed) {
        lv_label_set_text(time_label_, status.error.c_str());
    } else if (status.state == PlayerState::Loading || status.duration_us <= 0) {
        lv_label_set_text(time_label_, "loading...");
    } else {
        snprintf(text, sizeof(text), "%.1f / %.1f s   %.1f fps",
                 status.position_us / 1000000.0, status.duration_us / 1000000.0,
                 (double)video_presenter_fps());
        lv_label_set_text(time_label_, text);
    }

    const std::string decode = video_presenter_error();
    const char *audio = status.audio_note.empty() ? codec_name(status.audio_codec)
                                                  : status.audio_note.c_str();
    snprintf(text, sizeof(text), "audio %s   %s   %s", audio, name_.c_str(), decode.c_str());
    lv_label_set_text(status_label_, text);
}
