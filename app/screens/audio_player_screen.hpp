/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/media_cache.hpp"
#include "playback/player.hpp"
#include "playback/playlist.hpp"
#include "screen_manager.hpp"
#include "widgets.hpp"

#include <memory>
#include <string>
#include <vector>

class AudioPlayerScreen : public NavigationScreen, private ListDataSource {
public:
    explicit AudioPlayerScreen(std::shared_ptr<Playlist> playlist);
    ~AudioPlayerScreen() override;
    void build() override;
    void onEnter() override;
    void onExit() override;
    static void eject(const std::string &mount_point);

private:
    enum class Pane : uint8_t { Artwork, Playlist, Info, Settings };

    const std::string &name() const { return playlist_->current().name; }
    const std::string &path() const { return playlist_->current().path; }

    void buildContents();
    void buildArtwork(lv_obj_t *pane);
    void showPane(Pane pane);
    void showInfo();
    void buildPlaylist();
    void syncPlaylist();
    void rebindRow(std::size_t index);
    void rebindPath(const std::string &path);
    void updatePlaylistCount();
    void requestRows();
    static bool keepVisible(const char *path, void *ctx);
    void buildSeekRow(lv_obj_t *controls);
    void buildTransport(lv_obj_t *controls);
    void buildVolumeRow(lv_obj_t *controls);
    void relayout();
    void applyArtwork();
    void resetArtwork();
    void requestMeta();
    void prefetchNeighbours();
    std::string currentTitle() const;
    static void metaReady(const std::string &path);
    void openCurrent();
    bool advance(int delta, bool manual);
    void restart(bool resume);
    void updateTransport();
    void handleState();
    static void playerStateChanged();
    void setRepeatMode(RepeatMode mode);
    void setShuffle(bool shuffled);
    void setPlayIcon(bool playing);
    void setTime(lv_obj_t *label, int64_t *shown_s, int64_t us);
    void refresh();

    std::size_t rowCount() const override;
    int32_t rowHeight() const override;
    lv_obj_t *createRow(lv_obj_t *parent) override;
    void bindRow(lv_obj_t *row, std::size_t index) override;
    void didSelectRow(std::size_t index) override;

    std::shared_ptr<Playlist> playlist_;
    bool playing_ = false;
    bool scrubbing_ = false;
    bool awaiting_start_ = false;
    bool auto_opened_ = false;
    int skips_ = 0;
    bool landscape_ = false;
    RepeatMode repeat_ = RepeatMode::Off;
    Pane pane_ = Pane::Artwork;

    std::shared_ptr<const MediaEntry> meta_;
    ImageSize cover_size_;
    uint32_t token_ = 0;
    uint32_t rows_token_ = 0;
    std::size_t requested_first_ = SIZE_MAX;
    unsigned shown_order_ = 0;
    std::size_t shown_position_ = SIZE_MAX;
    int64_t list_scrolled_us_ = 0;
    std::vector<std::string> visible_paths_;
    int64_t prefetch_after_us_ = 0;
    bool prefetched_ = false;

    lv_obj_t *segment_ = nullptr;
    lv_obj_t *pane_box_ = nullptr;
    lv_obj_t *pane_view_ = nullptr;
    lv_obj_t *playlist_list_ = nullptr;
    lv_obj_t *playlist_count_ = nullptr;
    lv_obj_t *play_label_ = nullptr;
    lv_obj_t *next_button_ = nullptr;
    lv_obj_t *artwork_ = nullptr;
    lv_obj_t *artwork_icon_ = nullptr;
    lv_obj_t *artwork_image_ = nullptr;
    lv_obj_t *title_label_ = nullptr;
    lv_obj_t *repeat_button_ = nullptr;
    lv_obj_t *repeat_label_ = nullptr;
    lv_obj_t *shuffle_button_ = nullptr;
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
