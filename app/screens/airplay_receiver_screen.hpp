/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "airplay.hpp"
#include "widgets.hpp"

#include <atomic>
#include <memory>
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
        void on_airplay_state(airplay::State next) override { state = next; }
        void on_airplay_volume(float db) override;
    };

    class Output : public airplay::Output {
    public:
        bool open(uint32_t rate, uint8_t channels) override;
        void write(int16_t *pcm, std::size_t frames) override;
        void close() override;

    private:
        uint8_t channels_ = 0;
    };

    void refresh();
    bool startReceiver();
    void stopReceiver();

    std::shared_ptr<Events> events_ = std::make_shared<Events>();
    bool online_ = false;
    std::string shown_;
    lv_obj_t *status_label_ = nullptr;
    lv_timer_t *timer_ = nullptr;
};
