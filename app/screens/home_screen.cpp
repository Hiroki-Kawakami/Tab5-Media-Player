/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "home_screen.hpp"

#include <algorithm>
#include <cstring>
#include "media_player.hpp"
#include "resources.h"
#include "screen_manager.hpp"
#include "screens/airplay_receiver_screen.hpp"
#include "screens/video_input_screen.hpp"
#include "screens/home/display_page.hpp"
#include "screens/home/file_browser_page.hpp"
#include "screens/home/grouped_list.hpp"
#include "screens/home/sound_page.hpp"
#include "screens/home/wifi_page.hpp"
#include "widgets.hpp"

static constexpr int32_t kMenuWidth = 400;

const HomeScreen::MenuItem HomeScreen::kMenu[] = {
    {MenuId::SdCard, "Device", LV_SYMBOL_SD_CARD, nullptr, "SD Card", &HomeScreen::open_sd_card},
    {MenuId::UsbDrive, "Device", LV_SYMBOL_USB, nullptr, "USB Drive",
     &HomeScreen::open_usb_drive},
    {MenuId::VideoInput, "Device", TABLER_VIDEO, &icon_36, "Video Input",
     &HomeScreen::open_video_input},
    {MenuId::AirPlayReceiver, "Network", TABLER_CAST, &icon_36, "AirPlay Receiver",
     &HomeScreen::open_airplay_receiver},
    {MenuId::Display, "Settings", TABLER_SUN, &icon_36, "Display", &HomeScreen::open_display},
    {MenuId::Sound, "Settings", TABLER_VOLUME, &icon_36, "Sound", &HomeScreen::open_sound},
    {MenuId::Wifi, "Settings", TABLER_WIFI, &icon_36, "Wi-Fi", &HomeScreen::open_wifi},
};

static lv_obj_t *pane_create(lv_obj_t *parent, lv_color_t bg_color) {
    auto pane = lv_container_create(parent, bg_color);
    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(pane, LV_PCT(100), LV_PCT(100));
    lv_obj_set_scrollable(pane, false);
    return pane;
}

void HomeScreen::build() {
    lv_obj_set_scrollable(root_, false);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_style_pad_column(root_, 0, 0);
    menu_pane_ = pane_create(root_, lv_color_hex(0xeeeeee));
    build_menu();
    separator_ = lv_ver_separator_create(root_);
    page_pane_ = pane_create(root_, lv_color_white());
    lv_obj_set_width(page_pane_, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(page_pane_, 1);
    lv_obj_add_event_fn(root_, LV_EVENT_SIZE_CHANGED, [this](lv_event_t *) {
        if (is_landscape() != landscape_) navigate([] {});
    });
    layout();
}

void HomeScreen::push(std::shared_ptr<HomePage> page) {
    page->home_ = this;
    navigate([this, page] { stack_.push_back(page); });
}

void HomeScreen::pop() {
    navigate([this] {
        if (!stack_.empty()) stack_.pop_back();
        if (stack_.empty()) selected_.reset();
    });
}

void HomeScreen::eject(const std::string &mount_point) {
    auto under = [mount_point](const std::shared_ptr<HomePage> &page) {
        return page->is_under(mount_point);
    };
    if (std::none_of(stack_.begin(), stack_.end(), under)) return;
    navigate([this, under] {
        stack_.erase(std::find_if(stack_.begin(), stack_.end(), under), stack_.end());
        if (stack_.empty()) selected_.reset();
    });
}

void HomeScreen::refresh_menu() {
    int32_t scroll_y = lv_obj_get_scroll_y(menu_contents_);
    lv_obj_clean(menu_contents_);
    menu_rows_.clear();

    auto items = menu_items();
    lv_obj_t *section = nullptr;
    const char *section_title = nullptr;
    for (auto item : items) {
        if (!section_title || std::strcmp(item->section, section_title) != 0) {
            section = lv_grouped_section_create(menu_contents_, item->section);
            section_title = item->section;
        }
        auto row = lv_grouped_row_create(section, item->icon, item->label, item->icon_font);
        lv_obj_add_event_fn(row, LV_EVENT_CLICKED, [this, item](lv_event_t *) { select(*item); });
        menu_rows_.emplace_back(item->id, row);
    }
    update_menu_rows();
    lv_obj_update_layout(menu_contents_);
    lv_obj_scroll_to_y(menu_contents_, scroll_y, LV_ANIM_OFF);

    auto selected = [this](const MenuItem *item) { return item->id == selected_; };
    if (selected_ && std::none_of(items.begin(), items.end(), selected)) {
        navigate([this] {
            stack_.clear();
            selected_.reset();
        });
    }
}

void HomeScreen::onAppear() {
    if (visible_) visible_->on_appear();
}

/* A screen opening on top of the browser wants the card and the decoder now:
   whatever the rows behind it were still loading is given up here rather than
   left in front of the picture or the track the user just tapped. */
void HomeScreen::onDisappear() {
    if (visible_) visible_->on_disappear();
}

bool HomeScreen::is_landscape() const {
    return lv_obj_get_width(root_) > lv_obj_get_height(root_);
}

void HomeScreen::navigate(std::function<void()> change) {
    std::weak_ptr<Screen> weak = weak_from_this();
    lv_async_call([this, weak, change = std::move(change)] {
        if (weak.expired()) return;
        if (visible_) visible_->save_state();
        visible_ = nullptr;
        lv_obj_clean(page_pane_);
        change();
        layout();
    });
}

std::vector<const HomeScreen::MenuItem *> HomeScreen::menu_items() const {
    std::vector<const MenuItem *> items;
    for (auto &item : kMenu) {
        if (item.id == MenuId::UsbDrive && !media_player_usb_connected()) continue;
        if (item.id == MenuId::VideoInput && !media_player_camera()) continue;
        items.push_back(&item);
    }
    return items;
}

void HomeScreen::layout() {
    landscape_ = is_landscape();
    lv_obj_set_width(menu_pane_, landscape_ ? kMenuWidth : LV_PCT(100));
    lv_obj_set_hidden(menu_pane_, !landscape_ && !stack_.empty());
    lv_obj_set_hidden(separator_, !landscape_);
    lv_obj_set_hidden(page_pane_, !landscape_ && stack_.empty());
    update_menu_rows();
    build_page();
}

void HomeScreen::build_menu() {
    auto navigation = lv_navigation_create(menu_pane_);
    lv_navigation_title_create(navigation, "Media Player");

    menu_contents_ = lv_spacer_create(menu_pane_, LV_PCT(100), LV_SIZE_CONTENT, 1);
    lv_obj_set_flex_flow(menu_contents_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(menu_contents_, 24, 0);
    lv_obj_set_style_pad_row(menu_contents_, 12, 0);
    refresh_menu();
}

void HomeScreen::update_menu_rows() {
    for (auto &[id, row] : menu_rows_) {
        lv_grouped_row_set_arrow_visible(row, !landscape_);
        lv_obj_set_state(row, LV_STATE_CHECKED, landscape_ && selected_ == id);
    }
}

void HomeScreen::build_page() {
    if (stack_.empty()) {
        lv_obj_set_style_bg_color(page_pane_, lv_color_hex(0xeeeeee), 0);
        return;
    }
    lv_obj_set_style_bg_color(page_pane_, lv_color_white(), 0);

    auto navigation = lv_navigation_create(page_pane_, LV_NAVIGATION_STYLE_LIST);
    auto contents = lv_spacer_create(page_pane_, LV_PCT(100), LV_SIZE_CONTENT, 1);
    lv_obj_set_flex_flow(contents, LV_FLEX_FLOW_COLUMN);

    HomePage *page = stack_.back().get();
    if (!landscape_ || stack_.size() > 1) {
        lv_navigation_back_create(navigation, page->title().c_str(),
                                  [this](lv_event_t *) { pop(); });
    } else {
        lv_navigation_title_create(navigation, page->title().c_str());
    }
    page->build_navigation(navigation);
    page->build(contents);
    visible_ = page;
}

void HomeScreen::select(const MenuItem &item) {
    if (selected_ == item.id && !stack_.empty()) {
        navigate([this] { stack_.resize(1); });
        return;
    }
    auto page = (this->*item.open)();
    if (!page) return;
    page->home_ = this;
    navigate([this, id = item.id, page] {
        stack_.assign(1, page);
        selected_ = id;
    });
}

std::shared_ptr<HomePage> HomeScreen::open_sd_card() {
    esp_err_t err = media_player_mount_sd();
    if (err != ESP_OK) {
        show_mount_error("SD Card", "Failed to mount SD card", err);
        return nullptr;
    }
    return std::make_shared<FileBrowserPage>(kSdMountPoint, "SD Card");
}

std::shared_ptr<HomePage> HomeScreen::open_usb_drive() {
    esp_err_t err = media_player_mount_usb();
    if (err == ESP_ERR_NOT_FOUND) {
        show_mount_error("USB Drive", "No USB drive connected", ESP_OK);
        return nullptr;
    }
    if (err != ESP_OK) {
        show_mount_error("USB Drive", "Failed to mount USB drive", err);
        return nullptr;
    }
    return std::make_shared<FileBrowserPage>(kUsbMountPoint, "USB Drive");
}

std::shared_ptr<HomePage> HomeScreen::open_video_input() {
    auto camera = media_player_camera();
    if (!camera) return nullptr;
    navigate([this] {
        stack_.clear();
        selected_.reset();
    });
    screen_manager.push(std::make_shared<VideoInputScreen>(std::move(camera)));
    return nullptr;
}

std::shared_ptr<HomePage> HomeScreen::open_airplay_receiver() {
    navigate([this] {
        stack_.clear();
        selected_.reset();
    });
    screen_manager.push(std::make_shared<AirPlayReceiverScreen>());
    return nullptr;
}

std::shared_ptr<HomePage> HomeScreen::open_display() {
    return std::make_shared<DisplayPage>();
}

std::shared_ptr<HomePage> HomeScreen::open_sound() {
    return std::make_shared<SoundPage>();
}

std::shared_ptr<HomePage> HomeScreen::open_wifi() {
    return std::make_shared<WifiPage>();
}

void HomeScreen::show_mount_error(const char *title, const char *message, esp_err_t err) {
    std::string text = message;
    if (err != ESP_OK) text = text + "\n" + esp_err_to_name(err);
    auto modal = lv_modal_open(root_);
    lv_modal_title_create(modal, title);
    lv_modal_message_create(modal, text.c_str());
    lv_modal_button_create(modal, "Close", LV_MODAL_BUTTON_TYPE_PRIMARY,
                           [modal](lv_event_t *) { lv_modal_close(modal); });
}
