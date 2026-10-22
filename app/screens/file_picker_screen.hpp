/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "screens/file_browser.hpp"
#include "widgets.hpp"

#include <functional>
#include <string>
#include <vector>

struct FilePickerOptions {
    std::string start;
    /* Empty to reach every storage from the storage list. */
    std::string root;
    const char *confirm = "Use This Folder";
    /* Files of this kind can be picked by tapping them; None shows files disabled. */
    MediaKind file_kind = MediaKind::None;
    /* Directories that are shown disabled. */
    std::vector<std::string> excluded;
    std::function<bool(const std::string &)> can_pick;
    std::function<void(const std::string &)> on_pick;
};

class FilePickerScreen : public NavigationScreen, private FileBrowserDelegate {
public:
    explicit FilePickerScreen(FilePickerOptions options) : options_(std::move(options)) {}
    ~FilePickerScreen() override;
    void build() override;
    void onEnter() override;
    void onExit() override;
    void onAppear() override;
    void onDisappear() override;
    void back() override;
    static void eject(const std::string &mount_point);
    static void refresh_storages();

private:
    FilePickerOptions options_;
    FileBrowser browser_{this};
    lv_obj_t *back_button_ = nullptr;
    lv_obj_t *confirm_button_ = nullptr;

    void update_navigation();
    void pick(const std::string &path);

    bool is_enabled(const std::string &path, const DirectoryEntry &entry) const override;
    void did_open_file(std::size_t index) override;
    void did_change_directory() override;
};
