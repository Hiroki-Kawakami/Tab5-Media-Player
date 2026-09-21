/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "playback/player.hpp"
#include "screen_manager.hpp"
#include "widgets.hpp"

#include <string>

class AudioPlayerScreen : public NavigationScreen {
public:
    AudioPlayerScreen(std::string name, std::string path)
        : name_(std::move(name)), path_(std::move(path)) {}
    ~AudioPlayerScreen() override;
    void build() override;
    void onEnter() override;
    void onExit() override;
    static void eject(const std::string &mount_point);

private:
    enum class RepeatMode { Off, All, One };

    bool isLandscape() const;
    void buildContents();
    lv_obj_t *buildArtwork(lv_obj_t *parent, int32_t side);
    void buildTitle(lv_obj_t *parent);
    void buildSeekRow(lv_obj_t *parent);
    void buildTransport(lv_obj_t *parent);
    void buildVolumeRow(lv_obj_t *parent);
    void relayout();
    void applyArtwork();
    void setRepeatMode(RepeatMode mode);
    void setPlayIcon(bool playing);
    void setTime(lv_obj_t *label, int64_t *shown_s, int64_t us);
    void tick();
    void refresh();

    std::string name_;
    std::string path_;
    bool playing_ = false;
    bool scrubbing_ = false;
    bool auto_start_ = true;
    bool landscape_ = false;
    RepeatMode repeat_ = RepeatMode::Off;

    CoverArt cover_;

    lv_obj_t *play_label_ = nullptr;
    lv_obj_t *artwork_ = nullptr;
    int32_t artwork_side_ = 0;
    lv_obj_t *artwork_icon_ = nullptr;
    lv_obj_t *artwork_image_ = nullptr;
    lv_obj_t *title_label_ = nullptr;
    lv_obj_t *repeat_label_ = nullptr;
    lv_obj_t *seek_ = nullptr;
    lv_obj_t *elapsed_label_ = nullptr;
    lv_obj_t *total_label_ = nullptr;
    lv_obj_t *subtitle_label_ = nullptr;
    lv_obj_t *volume_label_ = nullptr;
    lv_obj_t *volume_slider_ = nullptr;
    lv_timer_t *timer_ = nullptr;

    int64_t shown_elapsed_s_ = -1;
    int64_t shown_total_s_ = -1;
    std::string shown_title_;
    std::string shown_subtitle_;
};
