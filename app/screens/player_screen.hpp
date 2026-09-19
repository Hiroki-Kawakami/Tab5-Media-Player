/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "bsp_types.h"
#include "screen_manager.hpp"
#include "widgets.hpp"

#include <string>

class PlayerScreen : public NavigationScreen {
public:
    PlayerScreen(std::string name, std::string path)
        : name_(std::move(name)), path_(std::move(path)) {}
    ~PlayerScreen() override;
    void build() override;
    void onEnter() override;
    void onExit() override;
    static void eject(const std::string &mount_point);

private:
    bool openOverlay();
    void closeOverlay();
    void rotate(bsp_rotation_t rotation);
    void buildOverlay(lv_obj_t *parent, bool portrait);
    void setLoopIndicator(bool on);
    void refresh();
    void showStartError(const std::string &message);

    std::string name_;
    std::string path_;
    bool playing_ = false;
    bool scrubbing_ = false;
    bool looping_ = false;
    bsp_rotation_t rotation_ = BSP_ROTATION_0;

    lv_display_t *overlay_ = nullptr;
    lv_obj_t *play_label_ = nullptr;
    lv_obj_t *loop_label_ = nullptr;
    lv_obj_t *progress_ = nullptr;
    lv_obj_t *time_label_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *volume_slider_ = nullptr;
    lv_timer_t *timer_ = nullptr;
};
