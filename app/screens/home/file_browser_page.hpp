/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "home_page.hpp"
#include "media/file_operation.hpp"
#include "media/image_codec.hpp"
#include "media/media_directory.hpp"
#include "playback/playlist.hpp"
#include "settings.hpp"
#include "widgets.hpp"

class FileBrowserPage : public HomePage,
                        public std::enable_shared_from_this<FileBrowserPage>,
                        private ListDataSource,
                        private GridDataSource {
public:
    FileBrowserPage(std::string path, std::string title);
    ~FileBrowserPage() override;
    const std::string &title() const override { return selecting_ ? selection_title_ : title_; }
    bool shows_back() const override { return !selecting_; }
    void build_navigation(lv_obj_t *navigation) override;
    void build(lv_obj_t *contents) override;
    void save_state() override;
    void on_appear() override;
    void on_disappear() override;
    bool is_under(const std::string &mount_point) const override;

private:
    std::string path_;
    std::string title_;
    PsramVector<DirectoryEntry> entries_;
    BrowserLayout layout_ = BrowserLayout::Compact;
    lv_obj_t *contents_ = nullptr;
    bool loaded_ = false;
    bool opened_ = false;
    lv_obj_t *list_ = nullptr;
    int32_t scroll_y_ = 0;
    std::size_t requested_first_ = SIZE_MAX;
    uint32_t token_ = 0;
    uint32_t idle_token_ = 0;
    bool prefetched_ = false;
    PsramVector<PsramString> visible_;
    bool selecting_ = false;
    PsramVector<bool> selected_;
    std::size_t selected_count_ = 0;
    std::string selection_title_;
    lv_obj_t *toolbar_ = nullptr;
    lv_obj_t *move_button_ = nullptr;
    lv_obj_t *copy_button_ = nullptr;
    lv_obj_t *rename_button_ = nullptr;
    lv_obj_t *delete_button_ = nullptr;
    std::vector<std::string> operation_paths_;
    lv_obj_t *progress_modal_ = nullptr;
    lv_obj_t *progress_label_ = nullptr;
    lv_obj_t *progress_bar_ = nullptr;
    lv_timer_t *progress_timer_ = nullptr;
    lv_obj_t *rename_modal_ = nullptr;
    lv_obj_t *rename_error_ = nullptr;
    lv_obj_t *keyboard_ = nullptr;

    void open_menu(lv_obj_t *anchor);
    void set_selecting(bool selecting);
    void toggle_selected(std::size_t index);
    void select_all(bool selected);
    void update_selection();
    void build_toolbar();
    void reload_entries();
    void rebind_entry(std::size_t index);
    std::vector<std::string> selected_paths() const;
    void confirm_delete();
    void start_delete();
    void open_progress(const char *title);
    void close_progress();
    void poll_operation();
    void finish_operation(const FileOperationStatus &status, const char *failure);
    void reload_directory();
    void open_rename();
    void close_rename();
    void submit_rename(const std::string &name);
    void post(std::function<void(FileBrowserPage &)> fn);
    void set_layout(BrowserLayout layout);
    ImageSize thumb_box() const;
    std::size_t first_visible_entry() const;
    void show_entry(std::size_t index);
    void bind_thumbnail(lv_obj_t *frame, std::size_t index);
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

    std::size_t cellCount() const override;
    int32_t cellWidth() const override;
    int32_t cellHeight() const override;
    int32_t columnSpacing() const override;
    int32_t rowSpacing() const override;
    lv_obj_t *createCell(lv_obj_t *parent) override;
    void bindCell(lv_obj_t *cell, std::size_t index) override;
    void didSelectCell(std::size_t index) override;
};
