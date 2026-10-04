/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
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
    void refresh_menu();

private:
    enum class MenuId { SdCard, UsbDrive, AirPlayReceiver, Display, Sound, Wifi };

    struct MenuItem {
        MenuId id;
        const char *section;
        const char *icon;
        const lv_font_t *icon_font;
        const char *label;
        std::shared_ptr<HomePage> (HomeScreen::*open)();
    };
    static const MenuItem kMenu[];

    std::vector<std::shared_ptr<HomePage>> stack_;
    std::optional<MenuId> selected_;
    bool landscape_ = false;
    HomePage *visible_ = nullptr;
    lv_obj_t *menu_pane_ = nullptr;
    lv_obj_t *menu_contents_ = nullptr;
    lv_obj_t *separator_ = nullptr;
    lv_obj_t *page_pane_ = nullptr;
    std::vector<std::pair<MenuId, lv_obj_t *>> menu_rows_;

    bool is_landscape() const;
    std::vector<const MenuItem *> menu_items() const;
    void navigate(std::function<void()> change);
    void layout();
    void build_menu();
    void update_menu_rows();
    void build_page();
    void select(const MenuItem &item);
    std::shared_ptr<HomePage> open_sd_card();
    std::shared_ptr<HomePage> open_usb_drive();
    std::shared_ptr<HomePage> open_airplay_receiver();
    std::shared_ptr<HomePage> open_display();
    std::shared_ptr<HomePage> open_sound();
    std::shared_ptr<HomePage> open_wifi();
    void show_mount_error(const char *title, const char *message, esp_err_t err);
};
