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
    bsp_rotation_t rotation() const { return rotation_; }
    void refresh();

private:
    enum class RepeatMode { Off, All, One };

    bool openOverlay();
    void closeOverlay();
    void rotate(bsp_rotation_t rotation);
    void buildTopBar(lv_obj_t *parent);
    void buildBottomBar(lv_obj_t *parent, bool portrait);
    void buildTransport(lv_obj_t *parent, bool repeat_only);
    void buildSeekRow(lv_obj_t *parent);
    void buildVolumeRow(lv_obj_t *parent);
    void setRepeatMode(RepeatMode mode);
    void setPlayIcon(bool playing);
    void setVolumeIcon(int32_t volume);
    void setTime(lv_obj_t *label, int64_t *shown_s, int64_t us);
    void tick();
    void showStartError(const std::string &message);

    std::string name_;
    std::string path_;
    bool playing_ = false;
    bool scrubbing_ = false;
    bool auto_start_ = true;
    bool stop_bars_shown_ = false;
    uint32_t auto_start_tick_ = 0;
    bool muted_ = false;
    RepeatMode repeat_ = RepeatMode::Off;
    bsp_rotation_t rotation_ = BSP_ROTATION_0;

    lv_display_t *top_ = nullptr;
    lv_display_t *bottom_ = nullptr;
    lv_obj_t *title_label_ = nullptr;
    lv_obj_t *play_label_ = nullptr;
    lv_obj_t *repeat_label_ = nullptr;
    lv_obj_t *seek_ = nullptr;
    lv_obj_t *elapsed_label_ = nullptr;
    lv_obj_t *total_label_ = nullptr;
    lv_obj_t *volume_label_ = nullptr;
    lv_obj_t *volume_slider_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    int64_t shown_elapsed_s_ = -1;
    int64_t shown_total_s_ = -1;
    std::string shown_message_;
};
