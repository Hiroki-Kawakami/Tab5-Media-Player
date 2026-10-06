/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "audio/capture_playback.hpp"
#include "bsp_types.h"
#include "screen_manager.hpp"
#include "usb_host_uvc.hpp"
#include "widgets.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <array>
#include <atomic>
#include <memory>
#include <string>

struct VideoInsets;

class VideoInputScreen : public NavigationScreen {
public:
    explicit VideoInputScreen(std::shared_ptr<usb_host::UvcDevice> camera);
    ~VideoInputScreen() override;
    void build() override;
    void onEnter() override;
    void onExit() override;
    static void unplugged();

private:
    enum class UiMode { Hidden, Bars, Settings };

    static constexpr std::size_t kSlots = 4;

    struct Held {
        usb_host::UvcDevice *camera = nullptr;
        usb_host::UvcFrame frame;
    };

    bool openOverlay();
    void closeOverlay();
    void buildUi();
    void buildBottomBar(lv_obj_t *parent);
    void rotate(bsp_rotation_t rotation);
    void setMode(UiMode mode);
    void requestMode(UiMode mode);
    VideoInsets insets() const;
    bool startCapture(std::string *error);
    void stopCapture();
    static void feedMain(void *arg);
    static void releaseFrame(void *ctx);
    void tick();
    void refresh();
    void showStartError(const std::string &message);

    std::shared_ptr<usb_host::UvcDevice> camera_;
    std::array<Held, kSlots> held_;
    CapturePlayback audio_;
    SemaphoreHandle_t feed_stopped_ = nullptr;
    std::atomic<bool> feed_quit_{ false };
    std::string title_;
    std::string error_;
    std::string shown_title_;
    UiMode mode_ = UiMode::Hidden;
    bsp_rotation_t rotation_ = BSP_ROTATION_0;

    lv_display_t *ui_ = nullptr;
    lv_obj_t *top_bar_ = nullptr;
    lv_obj_t *bottom_bar_ = nullptr;
    lv_obj_t *settings_ = nullptr;
    lv_obj_t *title_label_ = nullptr;
    lv_obj_t *volume_label_ = nullptr;
    lv_obj_t *volume_slider_ = nullptr;
    lv_timer_t *timer_ = nullptr;
};
