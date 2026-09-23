/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <memory>
#include <string>
#include <vector>
#include "home_page.hpp"
#include "media/media_directory.hpp"
#include "playback/playlist.hpp"
#include "widgets.hpp"

class FileBrowserPage : public HomePage, private ListDataSource {
public:
    FileBrowserPage(std::string path, std::string title);
    ~FileBrowserPage() override;
    const std::string &title() const override { return title_; }
    void build(lv_obj_t *contents) override;
    void save_state() override;
    void on_appear() override;
    void on_disappear() override;
    bool is_under(const std::string &mount_point) const override;

private:
    std::string path_;
    std::string title_;
    PsramVector<DirectoryEntry> entries_;
    bool loaded_ = false;
    bool opened_ = false;
    lv_obj_t *list_ = nullptr;
    int32_t scroll_y_ = 0;
    std::size_t requested_first_ = SIZE_MAX;
    uint32_t token_ = 0;
    uint32_t idle_token_ = 0;
    bool prefetched_ = false;
    PsramVector<PsramString> visible_;

    std::shared_ptr<Playlist> make_playlist(std::size_t index) const;
    std::string entry_path(std::size_t index) const;
    void request_visible();
    void prefetch_rest();
    void release_requests();
    static void meta_ready(const std::string &path);
    static bool keep_visible(const char *path, void *ctx);

    std::size_t rowCount() const override;
    int32_t rowHeight() const override;
    lv_obj_t *createRow(lv_obj_t *parent) override;
    void bindRow(lv_obj_t *row, std::size_t index) override;
    void didSelectRow(std::size_t index) override;
};
