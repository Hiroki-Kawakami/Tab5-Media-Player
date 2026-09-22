/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "home_screen.hpp"

#include <algorithm>
#include <cstring>
#include <iterator>
#include "bsp.h"
#include "media_player.hpp"
#include "resources.h"
#include "screens/home/display_page.hpp"
#include "screens/home/file_browser_page.hpp"
#include "screens/home/grouped_list.hpp"
#include "screens/home/sound_page.hpp"
#include "usb_msc.h"
#include "widgets.hpp"

static constexpr int32_t kMenuWidth = 400;

const HomeScreen::MenuItem HomeScreen::kMenu[] = {
    {"Storage", LV_SYMBOL_SD_CARD, nullptr, "SD Card", &HomeScreen::open_sd_card},
    {"Storage", LV_SYMBOL_USB, nullptr, "USB Drive", &HomeScreen::open_usb_drive},
    {"Settings", TABLER_SUN, &icon_36, "Display", &HomeScreen::open_display},
    {"Settings", TABLER_VOLUME, &icon_36, "Sound", &HomeScreen::open_sound},
};

static lv_obj_t *pane_create(lv_obj_t *parent, lv_color_t bg_color) {
    auto pane = lv_container_create(parent, bg_color);
    lv_obj_set_flex_flow(pane, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(pane, LV_PCT(100), LV_PCT(100));
    lv_obj_remove_flag(pane, LV_OBJ_FLAG_SCROLLABLE);
    return pane;
}

void HomeScreen::build() {
    lv_obj_remove_flag(root_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(root_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_set_style_pad_column(root_, 0, 0);
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
        if (stack_.empty()) selected_ = SIZE_MAX;
    });
}

void HomeScreen::eject(const std::string &mount_point) {
    auto under = [mount_point](const std::shared_ptr<HomePage> &page) {
        return page->is_under(mount_point);
    };
    if (std::none_of(stack_.begin(), stack_.end(), under)) return;
    navigate([this, under] {
        stack_.erase(std::find_if(stack_.begin(), stack_.end(), under), stack_.end());
        if (stack_.empty()) selected_ = SIZE_MAX;
    });
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
        lv_obj_clean(root_);
        change();
        layout();
    });
}

void HomeScreen::layout() {
    landscape_ = is_landscape();
    if (!landscape_) {
        auto pane = pane_create(root_, lv_color_white());
        if (stack_.empty()) {
            build_menu(pane);
        } else {
            build_page(pane);
        }
        return;
    }

    auto menu = pane_create(root_, lv_color_white());
    lv_obj_set_width(menu, kMenuWidth);
    build_menu(menu);
    lv_ver_separator_create(root_);
    auto page = pane_create(root_, lv_color_white());
    lv_obj_set_width(page, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(page, 1);
    build_page(page);
}

void HomeScreen::build_menu(lv_obj_t *pane) {
    lv_obj_set_style_bg_color(pane, lv_color_hex(0xeeeeee), 0);
    auto navigation = lv_navigation_create(pane);
    lv_navigation_title_create(navigation, "Tab5MediaPlayer");

    auto contents = lv_spacer_create(pane, LV_PCT(100), LV_SIZE_CONTENT, 1);
    lv_obj_set_flex_flow(contents, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(contents, 24, 0);
    lv_obj_set_style_pad_row(contents, 12, 0);

    lv_obj_t *section = nullptr;
    for (std::size_t i = 0; i < std::size(kMenu); i++) {
        if (i == 0 || std::strcmp(kMenu[i].section, kMenu[i - 1].section) != 0) {
            section = lv_grouped_section_create(contents, kMenu[i].section);
        }
        auto row = lv_grouped_row_create(section, kMenu[i].icon, kMenu[i].label,
                                         kMenu[i].icon_font);
        lv_grouped_row_set_arrow_visible(row, !landscape_);
        lv_obj_set_state(row, LV_STATE_CHECKED, landscape_ && selected_ == i);
        lv_obj_add_event_fn(row, LV_EVENT_CLICKED, [this, i](lv_event_t *) { select(i); });
    }
}

void HomeScreen::build_page(lv_obj_t *pane) {
    if (stack_.empty()) {
        lv_obj_set_style_bg_color(pane, lv_color_hex(0xeeeeee), 0);
        return;
    }

    auto navigation = lv_navigation_create(pane, LV_NAVIGATION_STYLE_LIST);
    auto contents = lv_spacer_create(pane, LV_PCT(100), LV_SIZE_CONTENT, 1);
    lv_obj_set_flex_flow(contents, LV_FLEX_FLOW_COLUMN);

    HomePage *page = stack_.back().get();
    if (!landscape_ || stack_.size() > 1) {
        lv_navigation_back_create(navigation, page->title().c_str(),
                                  [this](lv_event_t *) { pop(); });
    } else {
        lv_navigation_title_create(navigation, page->title().c_str());
    }
    page->build(contents);
    visible_ = page;
}

void HomeScreen::select(std::size_t index) {
    if (selected_ == index && !stack_.empty()) {
        navigate([this] { stack_.resize(1); });
        return;
    }
    auto page = (this->*kMenu[index].open)();
    if (!page) return;
    page->home_ = this;
    navigate([this, index, page] {
        stack_.assign(1, page);
        selected_ = index;
    });
}

std::shared_ptr<HomePage> HomeScreen::open_sd_card() {
    if (!bsp_sd_is_mounted()) {
        bsp_sd_mount_config_t config = {};
        config.psram_bounce_buffer = true;
        esp_err_t err = bsp_sd_mount(kSdMountPoint, &config);
        if (err != ESP_OK) {
            show_mount_error("SD Card", "Failed to mount SD card", err);
            return nullptr;
        }
    }
    return std::make_shared<FileBrowserPage>(kSdMountPoint, "SD Card");
}

std::shared_ptr<HomePage> HomeScreen::open_usb_drive() {
    if (!usb_msc_is_mounted()) {
        esp_err_t err = usb_msc_mount(kUsbMountPoint, 0);
        if (err == ESP_ERR_NOT_FOUND) {
            show_mount_error("USB Drive", "No USB drive connected", ESP_OK);
            return nullptr;
        }
        if (err != ESP_OK) {
            show_mount_error("USB Drive", "Failed to mount USB drive", err);
            return nullptr;
        }
    }
    return std::make_shared<FileBrowserPage>(kUsbMountPoint, "USB Drive");
}

std::shared_ptr<HomePage> HomeScreen::open_display() {
    return std::make_shared<DisplayPage>();
}

std::shared_ptr<HomePage> HomeScreen::open_sound() {
    return std::make_shared<SoundPage>();
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
