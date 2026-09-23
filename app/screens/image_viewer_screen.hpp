/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/media_cache.hpp"
#include "playback/playlist.hpp"
#include "screen_manager.hpp"
#include "widgets.hpp"

#include <cstddef>
#include <memory>
#include <string>

class ImageViewerScreen : public NavigationScreen {
public:
    explicit ImageViewerScreen(std::shared_ptr<Playlist> playlist)
        : playlist_(std::move(playlist)) {}
    ~ImageViewerScreen() override;
    void build() override;
    void onEnter() override;
    void onExit() override;
    static void eject(const std::string &mount_point);

private:
    enum class UiMode { Hidden, Bars, Slideshow, Settings, Info };

    const std::string &name() const { return playlist_->current().name; }
    const std::string &path() const { return playlist_->current().path; }

    void buildUi();
    void buildBottomBar(lv_obj_t *parent);
    void buildPanels();
    void populateInfo();
    void refreshInfo();
    void setMode(UiMode mode);
    void requestMode(UiMode mode);
    void requestLoad();
    void requestAdvance(int delta);
    void load();
    void prefetch();
    static void imageReady(const std::string &path);
    void showReady();
    bool showCached();
    bool showScaled();
    void showFramebuffer(int index, ImageSize size);
    void createImage();
    void setMessage(const std::string &message, bool failed);
    void updateTransport();
    void handlePress(lv_event_t *event);
    void handleMove(lv_event_t *event);
    void startSlideshow();
    void endSlideshow(std::size_t index);

    std::shared_ptr<Playlist> playlist_;
    UiMode mode_ = UiMode::Hidden;
    bool landscape_ = false;
    bool swiped_ = false;
    bool slideshow_running_ = false;
    lv_point_t press_ = {};
    ImageSize box_;
    uint32_t token_ = 0;
    uint32_t idle_token_ = 0;

    int shown_fb_ = -1;
    ImageSize shown_size_;
    std::string shown_path_;
    lv_obj_t *stage_ = nullptr;
    lv_obj_t *image_ = nullptr;
    lv_obj_t *message_ = nullptr;
    lv_obj_t *top_bar_ = nullptr;
    lv_obj_t *bottom_bar_ = nullptr;
    lv_obj_t *slideshow_ = nullptr;
    lv_obj_t *settings_ = nullptr;
    lv_obj_t *info_ = nullptr;
    lv_obj_t *title_label_ = nullptr;
    lv_obj_t *info_button_ = nullptr;
    lv_obj_t *slideshow_button_ = nullptr;
    lv_obj_t *panel_button_ = nullptr;
    lv_obj_t *prev_button_ = nullptr;
    lv_obj_t *next_button_ = nullptr;
    lv_obj_t *counter_label_ = nullptr;
};
