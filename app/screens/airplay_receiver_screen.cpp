/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "airplay_receiver_screen.hpp"
#include "audio/audio_output.hpp"
#include "screens/image_object.hpp"
#include "screens/media_controls.hpp"
#include "screens/player_layout.hpp"
#include "screens/player_panel.hpp"
#include "screens/video_player/settings_panel.hpp"
#include "resources.h"
#include "wifi_manager.hpp"
#include "bsp.h"
#include "esp_heap_caps.h"
#include "esp_mac.h"

#include <cstdio>
#include <cstring>

static constexpr const char *kReceiverName = "Tab5 Media Player";
static constexpr const char *kReceiverModel = "M5StackTab5";
static constexpr uint32_t kRefreshPeriodMs = 250;
static constexpr uint32_t kDecodeStackBytes = 16 * 1024;
static constexpr int32_t kSeekRange = 1000;
static constexpr int32_t kVolumeRange = 300;
static constexpr ImageSize kArtworkBox = { kPlayerArtworkSide, kPlayerArtworkSide };
/* LVGL draws through framebuffer 0 only. */
static constexpr int kArtworkFramebuffers[] = { 1, 2 };

static std::string format_names(const std::string &artist, const std::string &album) {
    if (artist.empty()) return album;
    if (album.empty()) return artist;
    return artist + " \u00b7 " + album;
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
    return audio_output_open(rate, 16, channels, AudioContent::Music) == ESP_OK;
}

void AirPlayReceiverScreen::Output::write(int16_t *pcm, std::size_t frames) {
    audio_output_write(pcm, frames * channels_ * sizeof(int16_t));
}

void AirPlayReceiverScreen::Output::close() { audio_output_close(); }

void AirPlayReceiverScreen::build() {
    lv_obj_add_event_fn(root_, LV_EVENT_SIZE_CHANGED, [this](lv_event_t *) {
        if (player_layout_landscape(root_) != landscape_) relayout();
    });
    buildContents();
}

void AirPlayReceiverScreen::relayout() {
    std::weak_ptr<Screen> weak = weak_from_this();
    lv_async_call([this, weak] {
        if (!weak.expired() && !exited()) buildContents();
    });
}

void AirPlayReceiverScreen::buildContents() {
    segment_ = nullptr;
    pane_box_ = nullptr;
    pane_view_ = nullptr;
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

    landscape_ = player_layout_landscape(root_);
    const PlayerLayout layout = player_layout_build(
        root_, "AirPlay Receiver", [this] { back(); },
        { TABLER_PHOTO, TABLER_INFO_CIRCLE, TABLER_ADJUSTMENTS_HORIZONTAL },
        [this](int index) {
            if ((int)pane_ != index) showPane((Pane)index);
        });
    navigation_ = layout.navigation;
    navigation_title_ = layout.back_label;
    contents_ = layout.controls;
    segment_ = layout.segment;
    pane_box_ = layout.pane;
    title_label_ = layout.title;
    subtitle_label_ = layout.subtitle;

    artwork_ = player_artwork_create(pane_box_);
    buildSeekRow(layout.controls);
    buildTransport(layout.controls);
    buildVolumeRow(layout.controls);
    showPane(pane_);
    refresh();
}

void AirPlayReceiverScreen::showPane(Pane pane) {
    pane_ = pane;
    player_segment_set_active(segment_, (int)pane_);
    if (pane_view_) {
        lv_obj_delete(pane_view_);
        pane_view_ = nullptr;
    }
    shown_info_.clear();
    lv_obj_set_hidden(artwork_, pane != Pane::Artwork);
    if (pane == Pane::Artwork) return;

    pane_view_ = player_pane_view_create(pane_box_);
    lv_setting_page_style(pane_view_);
    if (pane == Pane::Settings) {
        player_settings_sections_build(pane_view_, PlayerSettingsDisplay | PlayerSettingsSound,
                                       nullptr, false);
    }
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
        lv_obj_set_style_text_color(artwork_icon_, lv_color_hex(kPlayerArtworkIconColor), 0);
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

void AirPlayReceiverScreen::buildSeekRow(lv_obj_t *controls) {
    const PlayerSeekRow row = player_seek_row_build(controls, kSeekRange);
    elapsed_label_ = row.elapsed;
    seek_ = row.slider;
    total_label_ = row.total;
    lv_obj_set_clickable(seek_, false);
    lv_obj_set_style_opa(seek_, LV_OPA_TRANSP, LV_PART_KNOB);
}

void AirPlayReceiverScreen::buildTransport(lv_obj_t *controls) {
    const lv_color_t foreground = lv_color_hex(kPlayerForegroundColor);
    const PlayerRow row = player_row_build(controls);

    prev_button_ =
        media_icon_button(row.middle, 96, &icon_48, TABLER_PLAYER_TRACK_PREV, foreground);
    lv_obj_add_event_fn(prev_button_, LV_EVENT_CLICKED,
                        [](lv_event_t *) { airplay::remote(airplay::Command::Previous); });

    play_button_ =
        media_icon_button(row.middle, 120, &icon_72, TABLER_PLAYER_PLAY, foreground, &play_label_);
    lv_obj_add_event_fn(play_button_, LV_EVENT_CLICKED,
                        [](lv_event_t *) { airplay::remote(airplay::Command::PlayPause); });

    next_button_ =
        media_icon_button(row.middle, 96, &icon_48, TABLER_PLAYER_TRACK_NEXT, foreground);
    lv_obj_add_event_fn(next_button_, LV_EVENT_CLICKED,
                        [](lv_event_t *) { airplay::remote(airplay::Command::Next); });
}

void AirPlayReceiverScreen::buildVolumeRow(lv_obj_t *controls) {
    const lv_color_t foreground = lv_color_hex(kPlayerForegroundColor);
    const PlayerRow row = player_row_build(controls);
    lv_obj_t *icon = media_icon_button(row.left, kPlayerIconButton, &icon_36, TABLER_VOLUME,
                                       foreground, &volume_label_);
    lv_obj_set_clickable(icon, false);

    volume_slider_ =
        media_slider(row.middle, kVolumeRange, foreground, lv_color_hex(kPlayerTrackColor));
    lv_obj_set_flex_grow(volume_slider_, 1);
    lv_obj_set_clickable(volume_slider_, false);
    lv_obj_set_style_opa(volume_slider_, LV_OPA_TRANSP, LV_PART_KNOB);
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
            ((std::size_t)kArtworkBox.width * kArtworkBox.height * (rgb888 ? 3 : 2) + 63) &
            ~(std::size_t)63;
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
        player_scrolling_text_set(title_label_, title);
    }
    if (subtitle != shown_subtitle_) {
        shown_subtitle_ = subtitle;
        player_scrolling_text_set(subtitle_label_, subtitle);
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
    if (pane_ == Pane::Info) updateInfo(status, now);
}

void AirPlayReceiverScreen::updateInfo(const wifi::Status &status, const airplay::NowPlaying &now) {
    const bool connected =
        now.state == airplay::State::Connected || now.state == airplay::State::Playing;
    const char *state = !online_                               ? "Offline"
                      : now.state == airplay::State::Stopped   ? "Unavailable"
                      : now.state == airplay::State::Listening ? "Waiting"
                      : now.state == airplay::State::Playing   ? "Playing"
                                                               : "Connected";
    char text[64];
    std::vector<std::pair<std::string, std::string>> receiver = {
        { "Name", kReceiverName },
    };
    if (!status.ip.empty()) receiver.push_back({ "Address", status.ip });
    receiver.push_back({ "Status", state });

    std::vector<std::pair<std::string, std::string>> sender;
    std::vector<std::pair<std::string, std::string>> track;
    std::vector<std::pair<std::string, std::string>> audio;
    if (connected) {
        if (!now.sender_name.empty()) sender.push_back({ "Name", now.sender_name });
        if (!now.sender_address.empty()) sender.push_back({ "Address", now.sender_address });
        if (!now.title.empty()) track.push_back({ "Title", now.title });
        if (!now.artist.empty()) track.push_back({ "Artist", now.artist });
        if (!now.album.empty()) track.push_back({ "Album", now.album });
        if (now.duration_ms > 0) {
            media_format_time(text, sizeof(text), now.duration_ms / 1000);
            track.push_back({ "Duration", text });
        }
        if (now.codec) {
            audio.push_back({ "Codec", now.codec });
            snprintf(text, sizeof(text), "%.1f kHz", now.sample_rate / 1000.0);
            audio.push_back({ "Sample Rate", text });
            snprintf(text, sizeof(text), "%u%s", (unsigned)now.channels,
                     now.channels == 1 ? " (Mono)" : now.channels == 2 ? " (Stereo)" : "");
            audio.push_back({ "Channels", text });
        }
        if (now.has_volume) {
            if (now.volume_db <= airplay::kVolumeMute) {
                snprintf(text, sizeof(text), "Muted");
            } else {
                snprintf(text, sizeof(text), "%.1f dB", now.volume_db);
            }
            audio.push_back({ "Volume", text });
        }
    }

    std::string key;
    for (const auto *rows : { &receiver, &sender, &track, &audio }) {
        for (const auto &row : *rows) key += row.first + '\n' + row.second + '\n';
        key += '\n';
    }
    if (key == shown_info_) return;
    shown_info_ = key;

    lv_obj_clean(pane_view_);
    auto add_section = [this](const char *title,
                              const std::vector<std::pair<std::string, std::string>> &rows) {
        if (rows.empty()) return;
        lv_obj_t *section = lv_setting_section_create(pane_view_, title);
        lv_obj_update_layout(section);
        const int32_t width = lv_obj_get_content_width(section);
        for (const auto &row : rows) {
            panel_add_wide_row(section, row.first.c_str(), row.second.c_str(), width, nullptr);
        }
    };
    add_section("Receiver", receiver);
    add_section("Sender", sender);
    add_section("Now Playing", track);
    add_section("Audio", audio);
}
