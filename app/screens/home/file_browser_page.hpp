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
#include "playback/playlist.hpp"
#include "screens/file_browser.hpp"

class FileBrowserPage : public HomePage,
                        public std::enable_shared_from_this<FileBrowserPage>,
                        private FileBrowserDelegate {
public:
    explicit FileBrowserPage(const std::string &path);
    ~FileBrowserPage() override;
    const std::string &title() const override;
    bool shows_back() const override { return !browser_.selecting(); }
    bool can_go_back() const override { return browser_.can_go_back(); }
    void go_back() override { browser_.go_back(); }
    void go_to_root() override { browser_.go_to_root(); }
    void build_navigation(lv_obj_t *navigation) override;
    void build(lv_obj_t *contents) override;
    void save_state() override;
    void on_appear() override;
    void on_disappear() override;
    bool is_under(const std::string &mount_point) const override;

private:
    FileBrowser browser_{this};
    lv_obj_t *contents_ = nullptr;
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
    void update_selection();
    void build_toolbar();
    void reload_directory();
    void post(std::function<void(FileBrowserPage &)> fn);
    void show_error(const char *title, const std::string &path, int error);
    std::shared_ptr<Playlist> make_playlist(std::size_t index) const;

    void open_rename();
    void close_rename();
    void submit_rename(const std::string &name);

    void confirm_delete();
    void start_delete();
    void open_progress(const char *title);
    void close_progress();
    void poll_operation();
    void finish_operation(const FileOperationStatus &status, const char *failure);

    void open_move();
    void start_move(const std::vector<std::string> &sources, const std::string &destination);
    void run_move(const std::vector<std::string> &sources, const std::string &destination,
                  bool skip_existing);

    void did_open_file(std::size_t index) override;
    void did_change_directory() override;
    void did_change_selection() override;
};
