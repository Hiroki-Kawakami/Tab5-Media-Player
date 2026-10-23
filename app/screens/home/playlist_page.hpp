/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "home_page.hpp"
#include "media/image_codec.hpp"
#include "playback/playlist.hpp"
#include "widgets.hpp"

class PlaylistPage : public HomePage, private ListDataSource {
public:
    explicit PlaylistPage(const std::string &path);
    ~PlaylistPage() override;
    const std::string &title() const override { return file_.title; }
    void build(lv_obj_t *contents) override;
    void save_state() override;
    void on_appear() override;
    void on_disappear() override;
    bool is_under(const std::string &mount_point) const override;

private:
    std::string path_;
    PlaylistFile file_;
    bool opened_ = false;
    lv_obj_t *list_ = nullptr;
    int32_t scroll_y_ = 0;
    uint32_t token_ = 0;
    std::size_t requested_first_ = SIZE_MAX;
    std::vector<std::string> visible_paths_;

    bool tracks() const { return file_.kind == MediaKind::Audio; }
    ImageSize thumb_box() const;
    void observe();
    void release_requests();
    void request_rows();
    void rebind_path(const std::string &path);
    static void meta_ready(const std::string &path);
    static bool keep_visible(const char *path, void *ctx);

    std::size_t rowCount() const override;
    int32_t rowHeight() const override;
    lv_obj_t *createRow(lv_obj_t *parent) override;
    void bindRow(lv_obj_t *row, std::size_t index) override;
    void didSelectRow(std::size_t index) override;
};
