/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <string>
#include <vector>
#include "home_page.hpp"
#include "widgets.hpp"

class FileBrowserPage : public HomePage, private ListDataSource {
public:
    FileBrowserPage(std::string path, std::string title);
    const std::string &title() const override { return title_; }
    void build(lv_obj_t *contents) override;
    void save_state() override;
    bool is_under(const std::string &mount_point) const override;

private:
    struct Entry {
        std::string name;
        bool directory;
        bool playable;
    };

    std::string path_;
    std::string title_;
    std::vector<Entry> entries_;
    bool loaded_ = false;
    bool opened_ = false;
    lv_obj_t *list_ = nullptr;
    int32_t scroll_y_ = 0;

    bool load_entries();

    std::size_t rowCount() const override;
    int32_t rowHeight() const override;
    lv_obj_t *createRow(lv_obj_t *parent) override;
    void bindRow(lv_obj_t *row, std::size_t index) override;
    void didSelectRow(std::size_t index) override;
};
