/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "esp_err.h"
#include "screen.hpp"
#include "screens/home/home_page.hpp"

class HomeScreen : public Screen {
public:
    void build() override;
    void onAppear() override;
    void onDisappear() override;
    void push(std::shared_ptr<HomePage> page);
    void pop();
    void eject(const std::string &mount_point);

private:
    struct MenuItem {
        const char *section;
        const char *icon;
        const lv_font_t *icon_font;
        const char *label;
        std::shared_ptr<HomePage> (HomeScreen::*open)();
    };
    static const MenuItem kMenu[];

    std::vector<std::shared_ptr<HomePage>> stack_;
    std::size_t selected_ = SIZE_MAX;
    bool landscape_ = false;
    HomePage *visible_ = nullptr;

    bool is_landscape() const;
    void navigate(std::function<void()> change);
    void layout();
    void build_menu(lv_obj_t *pane);
    void build_page(lv_obj_t *pane);
    void select(std::size_t index);
    std::shared_ptr<HomePage> open_sd_card();
    std::shared_ptr<HomePage> open_usb_drive();
    std::shared_ptr<HomePage> open_display();
    std::shared_ptr<HomePage> open_sound();
    void show_mount_error(const char *title, const char *message, esp_err_t err);
};
