/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/media_directory.hpp"
#include "widgets.hpp"

#include <cstddef>
#include <functional>
#include <string>

class BgmPickerScreen : public NavigationScreen, private ListDataSource {
public:
    BgmPickerScreen(std::string start, std::function<void(const std::string &)> on_pick)
        : start_(std::move(start)), on_pick_(std::move(on_pick)) {}
    ~BgmPickerScreen() override;
    void build() override;
    void onEnter() override;
    void onExit() override;
    void back() override;
    static void eject(const std::string &mount_point);

private:
    void navigate(std::string path);
    void show(const std::string &path);
    void openStorage(const char *mount_point);
    void pick(const std::string &path);
    std::string entryPath(std::size_t index) const;

    std::size_t rowCount() const override;
    int32_t rowHeight() const override;
    lv_obj_t *createRow(lv_obj_t *parent) override;
    void bindRow(lv_obj_t *row, std::size_t index) override;
    void didSelectRow(std::size_t index) override;

    std::string start_;
    std::function<void(const std::string &)> on_pick_;
    std::string path_;
    PsramVector<DirectoryEntry> entries_;
    lv_obj_t *back_button_ = nullptr;
    lv_obj_t *use_button_ = nullptr;
};
