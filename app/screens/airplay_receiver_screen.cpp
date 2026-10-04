/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "airplay_receiver_screen.hpp"
#include "audio/audio_output.hpp"
#include "screens/image_object.hpp"
#include "screens/media_controls.hpp"
#include "resources.h"
#include "wifi_manager.hpp"
#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"

#include <cstring>

static constexpr const char *kReceiverName = "Tab5 Media Player";
static constexpr const char *kReceiverModel = "M5StackTab5";
static constexpr uint32_t kRefreshPeriodMs = 250;
static constexpr uint32_t kDecodeStackBytes = 16 * 1024;
static constexpr int32_t kSeekRange = 1000;
static constexpr int32_t kVolumeRange = 300;
static constexpr int32_t kPad = 24;
static constexpr int32_t kGap = 24;
static constexpr int32_t kTimeWidth = 100;
static constexpr int32_t kTitleBottomPad = 40;
static constexpr int32_t kArtworkGap = 64;
static constexpr int32_t kSeekRowHeight = 56;
static constexpr int32_t kIconButton = 72;
static constexpr int32_t kArtworkSide = 552;
static constexpr ImageSize kArtworkBox = { kArtworkSide, kArtworkSide };
static constexpr int32_t kArtworkRadius = 24;
/* LVGL draws through framebuffer 0 only. */
static constexpr int kArtworkFramebuffers[] = { 1, 2 };

static constexpr uint32_t kForegroundColor = 0x101010;
static constexpr uint32_t kTrackColor = 0xd0d0d0;
static constexpr uint32_t kArtworkColor = 0xdcdcdc;
static constexpr uint32_t kArtworkIconColor = 0x9e9e9e;
static constexpr uint32_t kSubtitleColor = 0x707070;

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

static std::string format_names(const std::string &artist, const std::string &album) {
    if (artist.empty()) return album;
    if (album.empty()) return artist;
    return artist + " - " + album;
}

static int32_t volume_value(float db) {
    if (db <= airplay::kVolumeMute) return 0;
    if (db < airplay::kVolumeMin) db = airplay::kVolumeMin;
    if (db > 0) db = 0;
    return (int32_t)((db - airplay::kVolumeMin) * 10 + 0.5f);
}

static const char *volume_icon(int32_t value, bool muted) {
    return muted                      ? TABLER_VOLUME_3
         : value <= 0                 ? TABLER_VOLUME_4
         : value < kVolumeRange / 2   ? TABLER_VOLUME_2
                                      : TABLER_VOLUME;
}

void AirPlayReceiverScreen::Events::on_airplay_volume(float db) {
    int volume = 0;
    if (db > airplay::kVolumeMute) {
        if (db < airplay::kVolumeMin) db = airplay::kVolumeMin;
        if (db > 0) db = 0;
        volume = 100 + (int)(db * 2.5f - 0.5f);
    }
    audio_output_apply_volume(volume);
}

void AirPlayReceiverScreen::Events::on_airplay_artwork(std::shared_ptr<const uint8_t> data,
                                                       std::size_t size) {
    std::lock_guard<std::mutex> guard(lock);
    artwork = std::move(data);
    artwork_size = size;
    artwork_serial++;
}

bool AirPlayReceiverScreen::Output::open(uint32_t rate, uint8_t channels) {
    channels_ = channels;
    return audio_output_open(rate, 16, channels) == ESP_OK;
}

void AirPlayReceiverScreen::Output::write(int16_t *pcm, std::size_t frames) {
    audio_output_write(pcm, frames * channels_ * sizeof(int16_t));
}

void AirPlayReceiverScreen::Output::close() { audio_output_close(); }

void AirPlayReceiverScreen::build() {
    createNavigation("AirPlay Receiver", LV_NAVIGATION_STYLE_DEFAULT | LV_NAVIGATION_STYLE_BACK);
    lv_obj_set_style_bg_color(root_, lv_color_white(), 0);
    lv_obj_add_event_fn(root_, LV_EVENT_SIZE_CHANGED, [this](lv_event_t *) {
        if (isLandscape() != landscape_) relayout();
    });
    buildContents();
}

bool AirPlayReceiverScreen::isLandscape() const {
    return lv_obj_get_width(root_) > lv_obj_get_height(root_);
}

void AirPlayReceiverScreen::relayout() {
    std::weak_ptr<Screen> weak = weak_from_this();
    lv_async_call([this, weak] {
        if (!weak.expired() && !exited()) buildContents();
    });
}

void AirPlayReceiverScreen::buildContents() {
    lv_obj_clean(contents_);
    artwork_ = nullptr;
    artwork_icon_ = nullptr;
    artwork_image_ = nullptr;
    shown_icon_ = nullptr;
    title_label_ = nullptr;
    subtitle_label_ = nullptr;
    seek_ = nullptr;
    elapsed_label_ = nullptr;
    total_label_ = nullptr;
    prev_button_ = nullptr;
    play_button_ = nullptr;
    play_label_ = nullptr;
    next_button_ = nullptr;
    volume_label_ = nullptr;
    volume_slider_ = nullptr;
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

    buildArtwork(contents_);
    if (!landscape_) lv_obj_move_to_index(artwork_, 0);
    refresh();
}

void AirPlayReceiverScreen::buildArtwork(lv_obj_t *parent) {
    artwork_ = lv_container_create(parent, lv_color_hex(kArtworkColor));
    lv_obj_set_size(artwork_, kArtworkSide, kArtworkSide);
    lv_obj_set_style_radius(artwork_, kArtworkRadius, 0);
    lv_obj_set_style_clip_corner(artwork_, true, 0);
    lv_obj_remove_flag(artwork_, LV_OBJ_FLAG_SCROLLABLE);
}

void AirPlayReceiverScreen::showArtworkIcon(const char *icon) {
    if (!artwork_) return;
    if (artwork_image_) {
        lv_obj_delete(artwork_image_);
        artwork_image_ = nullptr;
    }
    if (!artwork_icon_) {
        artwork_icon_ = lv_label_create(artwork_);
        lv_obj_set_style_text_font(artwork_icon_, &icon_120, 0);
        lv_obj_set_style_text_color(artwork_icon_, lv_color_hex(kArtworkIconColor), 0);
        shown_icon_ = nullptr;
    }
    if (icon != shown_icon_) {
        shown_icon_ = icon;
        lv_label_set_text(artwork_icon_, icon);
        lv_obj_center(artwork_icon_);
    }
}

void AirPlayReceiverScreen::showArtworkImage() {
    if (!artwork_ || (artwork_image_ && image_framebuffer_ == shown_framebuffer_)) return;
    auto *buffer = static_cast<uint8_t *>(bsp_display_get_frame_buffer(shown_framebuffer_));
    const bool rgb888 = bsp_display_get_pixel_format() == BSP_PIXEL_FORMAT_RGB888;
    lv_obj_t *image = image_object_create(artwork_, buffer, shown_size_, rgb888);
    if (!image) return;
    if (artwork_image_) lv_obj_delete(artwork_image_);
    artwork_image_ = image;
    image_framebuffer_ = shown_framebuffer_;
    if (artwork_icon_) {
        lv_obj_delete(artwork_icon_);
        artwork_icon_ = nullptr;
        shown_icon_ = nullptr;
    }
}

void AirPlayReceiverScreen::updateArtwork(bool connected) {
    if (!decoding_ && decode_pending_) {
        decode_pending_ = false;
        shown_framebuffer_ = decoded_size_.valid() ? decode_framebuffer_ : 0;
        shown_size_ = decoded_size_;
    }
    const uint32_t serial = events_->artwork_serial;
    if (!decoding_ && serial != requested_serial_) {
        std::shared_ptr<const uint8_t> data;
        std::size_t size = 0;
        {
            std::lock_guard<std::mutex> guard(events_->lock);
            data = events_->artwork;
            size = events_->artwork_size;
            requested_serial_ = events_->artwork_serial;
        }
        if (data && decode_wake_) {
            decode_framebuffer_ = shown_framebuffer_ == kArtworkFramebuffers[0]
                                      ? kArtworkFramebuffers[1]
                                      : kArtworkFramebuffers[0];
            decode_data_ = std::move(data);
            decode_size_ = size;
            decode_pending_ = true;
            decoding_ = true;
            xSemaphoreGive(decode_wake_);
        } else {
            shown_framebuffer_ = 0;
        }
    }
    if (!connected) {
        showArtworkIcon(TABLER_CAST);
    } else if (shown_framebuffer_) {
        showArtworkImage();
    } else if (!decoding_) {
        showArtworkIcon(TABLER_MUSIC);
    }
}

void AirPlayReceiverScreen::buildTitle(lv_obj_t *parent) {
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
    lv_label_set_text(title_label_, "");

    subtitle_label_ = lv_label_create(box);
    lv_obj_set_width(subtitle_label_, lv_pct(100));
    lv_obj_set_height(subtitle_label_, lv_font_get_line_height(lv_widgets_resolved_font(LV_WIDGETS_FONT_BODY)));
    lv_obj_set_font_role(subtitle_label_, LV_WIDGETS_FONT_BODY);
    lv_obj_set_style_text_align(subtitle_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(subtitle_label_, lv_color_hex(kSubtitleColor), 0);
    lv_label_set_long_mode(subtitle_label_, LV_LABEL_LONG_MODE_DOTS);
    lv_label_set_text(subtitle_label_, "");
}

void AirPlayReceiverScreen::buildSeekRow(lv_obj_t *parent) {
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
    lv_obj_remove_flag(seek_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(seek_, LV_OPA_TRANSP, LV_PART_KNOB);

    total_label_ = lv_label_create(row);
    lv_obj_set_width(total_label_, kTimeWidth);
    lv_obj_set_style_text_align(total_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_font_role(total_label_, LV_WIDGETS_FONT_BODY);
}

void AirPlayReceiverScreen::buildTransport(lv_obj_t *parent) {
    const lv_color_t foreground = lv_color_hex(kForegroundColor);
    lv_obj_t *outer = create_row(parent, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(outer, 0, 0);
    lv_spacer_create(outer, kTimeWidth, 1);

    lv_obj_t *row = create_row(outer, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_flex_grow(row, 1);
    lv_obj_set_width(row, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_column(row, 16, 0);

    prev_button_ = media_icon_button(row, 96, &icon_48, TABLER_PLAYER_TRACK_PREV, foreground);
    lv_obj_add_event_fn(prev_button_, LV_EVENT_CLICKED,
                        [](lv_event_t *) { airplay::remote(airplay::Command::Previous); });

    play_button_ =
        media_icon_button(row, 120, &icon_72, TABLER_PLAYER_PLAY, foreground, &play_label_);
    lv_obj_add_event_fn(play_button_, LV_EVENT_CLICKED,
                        [](lv_event_t *) { airplay::remote(airplay::Command::PlayPause); });

    next_button_ = media_icon_button(row, 96, &icon_48, TABLER_PLAYER_TRACK_NEXT, foreground);
    lv_obj_add_event_fn(next_button_, LV_EVENT_CLICKED,
                        [](lv_event_t *) { airplay::remote(airplay::Command::Next); });

    lv_spacer_create(outer, kTimeWidth, 1);
}

void AirPlayReceiverScreen::buildVolumeRow(lv_obj_t *parent) {
    const lv_color_t foreground = lv_color_hex(kForegroundColor);
    lv_obj_t *row = create_row(parent, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 16, 0);

    lv_obj_t *left = lv_container_create(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(left, kTimeWidth, LV_SIZE_CONTENT);
    lv_obj_t *icon = media_icon_button(left, kIconButton, &icon_36, TABLER_VOLUME, foreground,
                                       &volume_label_);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE);

    volume_slider_ = media_slider(row, kVolumeRange, foreground, lv_color_hex(kTrackColor));
    lv_obj_set_flex_grow(volume_slider_, 1);
    lv_obj_remove_flag(volume_slider_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(volume_slider_, LV_OPA_TRANSP, LV_PART_KNOB);

    lv_spacer_create(row, kTimeWidth, 1);
}

void AirPlayReceiverScreen::setTime(lv_obj_t *label, int64_t *shown_s, int64_t ms) {
    const int64_t seconds = ms < 0 ? -1 : ms / 1000;
    if (seconds == *shown_s) return;
    *shown_s = seconds;
    char text[16];
    media_format_time(text, sizeof(text), seconds);
    lv_label_set_text(label, text);
}

void AirPlayReceiverScreen::setVolume(const airplay::NowPlaying &now) {
    const bool muted = now.has_volume && now.volume_db <= airplay::kVolumeMute;
    const int32_t value = now.has_volume ? volume_value(now.volume_db) : 0;
    lv_slider_set_value(volume_slider_, value, LV_ANIM_OFF);
    const char *icon = volume_icon(value, muted);
    if (strcmp(lv_label_get_text(volume_label_), icon) != 0) lv_label_set_text(volume_label_, icon);
}

void AirPlayReceiverScreen::onEnter() {
    wifi::manager().set_power_save(wifi::PowerSave::None);
    startDecoder();
    refresh();
    timer_ = lv_timer_create([](lv_timer_t *timer) {
        static_cast<AirPlayReceiverScreen *>(lv_timer_get_user_data(timer))->refresh();
    }, kRefreshPeriodMs, this);
}

void AirPlayReceiverScreen::onExit() {
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    stopReceiver();
    stopDecoder();
    wifi::manager().set_power_save(wifi::PowerSave::Default);
}

AirPlayReceiverScreen::~AirPlayReceiverScreen() {
    if (timer_) lv_timer_delete(timer_);
}

bool AirPlayReceiverScreen::startReceiver() {
    airplay::Config config;
    config.name = kReceiverName;
    config.model = kReceiverModel;
    config.output = std::make_shared<Output>();
    esp_read_mac(config.mac, ESP_MAC_BASE);
    return airplay::start(config, events_);
}

void AirPlayReceiverScreen::stopReceiver() {
    airplay::stop();
    audio_output_apply_volume(audio_output_volume());
}

bool AirPlayReceiverScreen::startDecoder() {
    decode_wake_ = xSemaphoreCreateBinary();
    decode_stopped_ = xSemaphoreCreateBinary();
    decode_quit_ = false;
#ifdef ESP_PLATFORM
    const BaseType_t created = xTaskCreatePinnedToCoreWithCaps(
        decodeMain, "airplay_art", kDecodeStackBytes, this, 2, nullptr, tskNO_AFFINITY,
        MALLOC_CAP_SPIRAM);
#else
    const BaseType_t created =
        xTaskCreate(decodeMain, "airplay_art", kDecodeStackBytes, this, 2, nullptr);
#endif
    if (created == pdPASS) return true;
    vSemaphoreDelete(decode_wake_);
    vSemaphoreDelete(decode_stopped_);
    decode_wake_ = nullptr;
    decode_stopped_ = nullptr;
    return false;
}

void AirPlayReceiverScreen::stopDecoder() {
    if (!decode_wake_) return;
    decode_quit_ = true;
    xSemaphoreGive(decode_wake_);
    xSemaphoreTake(decode_stopped_, portMAX_DELAY);
    vSemaphoreDelete(decode_wake_);
    vSemaphoreDelete(decode_stopped_);
    decode_wake_ = nullptr;
    decode_stopped_ = nullptr;
    decode_data_.reset();
}

void AirPlayReceiverScreen::decodeMain(void *arg) {
    auto *self = static_cast<AirPlayReceiverScreen *>(arg);
    while (true) {
        xSemaphoreTake(self->decode_wake_, portMAX_DELAY);
        if (self->decode_quit_) break;
        auto *buffer =
            static_cast<uint8_t *>(bsp_display_get_frame_buffer(self->decode_framebuffer_));
        const bool rgb888 = bsp_display_get_pixel_format() == BSP_PIXEL_FORMAT_RGB888;
        const bsp_size_t panel = bsp_display_get_size();
        const std::size_t capacity = (std::size_t)panel.width * panel.height * (rgb888 ? 3 : 2);
        const std::size_t artwork_bytes =
            ((std::size_t)kArtworkSide * kArtworkSide * (rgb888 ? 3 : 2) + 63) & ~(std::size_t)63;
        ImageSize size;
        if (!image_decode_to_fit(self->decode_data_.get(), self->decode_size_, kArtworkBox, rgb888,
                                 buffer, artwork_bytes, buffer + artwork_bytes,
                                 capacity - artwork_bytes, &size)) {
            size = {};
        }
        self->decode_data_.reset();
        self->decoded_size_ = size;
        self->decoding_ = false;
    }
    xSemaphoreGive(self->decode_stopped_);
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

void AirPlayReceiverScreen::refresh() {
    const wifi::Status status = wifi::manager().status();
    const bool online = status.state == wifi::State::Connected && !status.ip.empty();
    if (online != online_) {
        online_ = online;
        if (online) {
            startReceiver();
        } else {
            stopReceiver();
        }
    }
    if (!seek_) return;

    const airplay::NowPlaying now = airplay::now_playing();
    const bool connected =
        now.state == airplay::State::Connected || now.state == airplay::State::Playing;
    updateArtwork(connected);

    std::string title = kReceiverName;
    std::string subtitle;
    if (!online) {
        subtitle = status.state == wifi::State::Off ? "Wi-Fi is off" : "Not connected to Wi-Fi";
    } else if (now.state == airplay::State::Stopped) {
        subtitle = "AirPlay is unavailable";
    } else if (!connected) {
        subtitle = "Waiting for AirPlay on " + status.ip;
    } else {
        if (!now.title.empty()) title = now.title;
        subtitle = format_names(now.artist, now.album);
        if (subtitle.empty()) subtitle = "Connected";
    }
    if (title != shown_title_) {
        shown_title_ = title;
        lv_label_set_text(title_label_, title.c_str());
    }
    if (subtitle != shown_subtitle_) {
        shown_subtitle_ = subtitle;
        lv_label_set_text(subtitle_label_, subtitle.c_str());
    }

    const bool known = connected && now.duration_ms > 0;
    setTime(total_label_, &shown_total_s_, known ? now.duration_ms : -1);
    setTime(elapsed_label_, &shown_elapsed_s_, known ? now.position_ms : -1);
    const int64_t value = known && now.position_ms > 0 ? now.position_ms * kSeekRange / now.duration_ms : 0;
    lv_slider_set_value(seek_, (int32_t)value, LV_ANIM_OFF);

    const bool playing = now.state == airplay::State::Playing;
    const char *icon = playing ? TABLER_PLAYER_PAUSE : TABLER_PLAYER_PLAY;
    if (strcmp(lv_label_get_text(play_label_), icon) != 0) lv_label_set_text(play_label_, icon);
    for (lv_obj_t *button : { prev_button_, play_button_, next_button_ }) {
        lv_obj_set_state(button, LV_STATE_DISABLED, !now.remote);
    }
    setVolume(now);
}
