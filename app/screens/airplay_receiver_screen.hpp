/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "airplay.hpp"
#include "media/image_codec.hpp"
#include "widgets.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>

class AirPlayReceiverScreen : public NavigationScreen {
public:
    ~AirPlayReceiverScreen() override;
    void build() override;
    void onEnter() override;
    void onExit() override;

private:
    class Events : public airplay::Listener {
    public:
        std::atomic<airplay::State> state{ airplay::State::Stopped };
        std::mutex lock;
        std::shared_ptr<const uint8_t> artwork;
        std::size_t artwork_size = 0;
        std::atomic<uint32_t> artwork_serial{ 0 };

        void on_airplay_state(airplay::State next) override { state = next; }
        void on_airplay_volume(float db) override;
        void on_airplay_artwork(std::shared_ptr<const uint8_t> data, std::size_t size) override;
    };

    class Output : public airplay::Output {
    public:
        bool open(uint32_t rate, uint8_t channels) override;
        void write(int16_t *pcm, std::size_t frames) override;
        void close() override;

    private:
        uint8_t channels_ = 0;
    };

    bool isLandscape() const;
    void relayout();
    void buildContents();
    void buildArtwork(lv_obj_t *parent);
    void buildTitle(lv_obj_t *parent);
    void buildSeekRow(lv_obj_t *parent);
    void buildTransport(lv_obj_t *parent);
    void buildVolumeRow(lv_obj_t *parent);
    void showArtworkIcon(const char *icon);
    void showArtworkImage();
    void updateArtwork(bool connected);
    void setTime(lv_obj_t *label, int64_t *shown_s, int64_t ms);
    void setVolume(const airplay::NowPlaying &now);
    void refresh();
    bool startReceiver();
    void stopReceiver();
    bool startDecoder();
    void stopDecoder();
    static void decodeMain(void *arg);

    std::shared_ptr<Events> events_ = std::make_shared<Events>();
    bool online_ = false;
    bool landscape_ = false;

    SemaphoreHandle_t decode_wake_ = nullptr;
    SemaphoreHandle_t decode_stopped_ = nullptr;
    std::atomic<bool> decode_quit_{ false };
    std::atomic<bool> decoding_{ false };
    std::shared_ptr<const uint8_t> decode_data_;
    std::size_t decode_size_ = 0;
    ImageSize decoded_size_;
    uint32_t requested_serial_ = 0;
    uint32_t shown_serial_ = 0;

    lv_obj_t *artwork_ = nullptr;
    lv_obj_t *artwork_icon_ = nullptr;
    lv_obj_t *artwork_image_ = nullptr;
    const char *shown_icon_ = nullptr;
    lv_obj_t *title_label_ = nullptr;
    lv_obj_t *subtitle_label_ = nullptr;
    lv_obj_t *seek_ = nullptr;
    lv_obj_t *elapsed_label_ = nullptr;
    lv_obj_t *total_label_ = nullptr;
    lv_obj_t *prev_button_ = nullptr;
    lv_obj_t *play_button_ = nullptr;
    lv_obj_t *play_label_ = nullptr;
    lv_obj_t *next_button_ = nullptr;
    lv_obj_t *volume_label_ = nullptr;
    lv_obj_t *volume_slider_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    int64_t shown_elapsed_s_ = -2;
    int64_t shown_total_s_ = -2;
    std::string shown_title_;
    std::string shown_subtitle_;
};
