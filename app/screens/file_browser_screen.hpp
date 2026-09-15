/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <string>
#include <vector>
#include "screen_manager.hpp"
#include "widgets.hpp"

class FileBrowserScreen : public NavigationScreen, private ListDataSource {
public:
    FileBrowserScreen(std::string path, std::string title);
    void build() override;

private:
    struct Entry {
        std::string name;
        bool directory;
        bool playable;
    };

    std::string path_;
    std::string title_;
    std::vector<Entry> entries_;

    bool load_entries();

    std::size_t rowCount() const override;
    int32_t rowHeight() const override;
    lv_obj_t *createRow(lv_obj_t *parent) override;
    void bindRow(lv_obj_t *row, std::size_t index) override;
    void didSelectRow(std::size_t index) override;
};
