/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "airplay_receiver_screen.hpp"
#include "resources.h"
#include "settings.hpp"
#include "wifi_manager.hpp"
#include "bsp.h"
#include "esp_mac.h"

static constexpr const char *kReceiverName = "Tab5 Media Player";
static constexpr const char *kReceiverModel = "M5StackTab5";
static constexpr uint32_t kRefreshPeriodMs = 250;
static constexpr int32_t kGap = 24;
static constexpr uint32_t kIconColor = 0x9e9e9e;
static constexpr uint32_t kPlayingColor = 0x2196f3;
static constexpr uint32_t kMutedColor = 0x808080;

void AirPlayReceiverScreen::Events::on_airplay_volume(float db) {
    int volume = 0;
    if (db > airplay::kVolumeMute) {
        if (db < airplay::kVolumeMin) db = airplay::kVolumeMin;
        if (db > 0) db = 0;
        volume = 100 + (int)(db * 2.5f - 0.5f);
    }
    bsp_audio_set_volume(volume);
}

bool AirPlayReceiverScreen::Output::open(uint32_t rate, uint8_t channels) {
    channels_ = channels;
    return bsp_audio_open(rate, 16, channels) == ESP_OK;
}

void AirPlayReceiverScreen::Output::write(int16_t *pcm, std::size_t frames) {
    bsp_audio_write(pcm, frames * channels_ * sizeof(int16_t));
}

void AirPlayReceiverScreen::Output::close() { bsp_audio_close(); }

void AirPlayReceiverScreen::build() {
    createNavigation("AirPlay Receiver", LV_NAVIGATION_STYLE_DEFAULT | LV_NAVIGATION_STYLE_BACK);
    lv_obj_set_style_bg_color(root_, lv_color_white(), 0);
    lv_obj_set_flex_flow(contents_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(contents_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(contents_, kGap, 0);

    auto icon = lv_label_create(contents_);
    lv_label_set_text(icon, TABLER_CAST);
    lv_obj_set_style_text_font(icon, &icon_120, 0);
    lv_obj_set_style_text_color(icon, lv_color_hex(kIconColor), 0);

    auto name = lv_label_create(contents_);
    lv_obj_set_font_role(name, LV_WIDGETS_FONT_HEADING);
    lv_label_set_text(name, kReceiverName);

    status_label_ = lv_label_create(contents_);
    lv_obj_set_font_role(status_label_, LV_WIDGETS_FONT_BODY);
    shown_.clear();
    refresh();
}

void AirPlayReceiverScreen::onEnter() {
    wifi::manager().set_power_save(wifi::PowerSave::None);
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
    bsp_audio_set_volume(settings_volume());
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

    std::string text;
    uint32_t color = kMutedColor;
    if (!online) {
        text = status.state == wifi::State::Off ? "Wi-Fi is off" : "Not connected to Wi-Fi";
    } else {
        switch (events_->state.load()) {
        case airplay::State::Stopped:
            text = "AirPlay is unavailable";
            break;
        case airplay::State::Listening:
            text = "Waiting for AirPlay on " + status.ip;
            break;
        case airplay::State::Connected:
            text = "Connected";
            break;
        case airplay::State::Playing:
            text = "Playing";
            color = kPlayingColor;
            break;
        }
    }
    if (!status_label_ || text == shown_) return;
    shown_ = text;
    lv_label_set_text(status_label_, text.c_str());
    lv_obj_set_style_text_color(status_label_, lv_color_hex(color), 0);
}
