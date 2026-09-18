/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "home_screen.hpp"

#include <string>
#include "bsp.h"
#include "file_browser_screen.hpp"
#include "media_player.hpp"
#include "usb_msc.h"

void HomeScreen::build() {
    createNavigation("Tab5-Media-Player");

    auto sd_card = lv_button_create(contents_, LV_BUTTON_STYLE_PRIMARY);
    lv_obj_set_width(sd_card, LV_PCT(100));
    lv_button_set_text(sd_card, LV_SYMBOL_SD_CARD "  SD Card");
    lv_obj_add_event_fn(sd_card, LV_EVENT_CLICKED, [this](lv_event_t *) { open_sd_card(); });

    auto usb_drive = lv_button_create(contents_, LV_BUTTON_STYLE_PRIMARY);
    lv_obj_set_width(usb_drive, LV_PCT(100));
    lv_button_set_text(usb_drive, LV_SYMBOL_USB "  USB Drive");
    lv_obj_add_event_fn(usb_drive, LV_EVENT_CLICKED, [this](lv_event_t *) { open_usb_drive(); });
}

void HomeScreen::open_sd_card() {
    if (!bsp_sd_is_mounted()) {
        bsp_sd_mount_config_t config = {};
        config.psram_bounce_buffer = true;
        esp_err_t err = bsp_sd_mount(kSdMountPoint, &config);
        if (err != ESP_OK) {
            show_mount_error("SD Card", "Failed to mount SD card", err);
            return;
        }
    }
    screen_manager.push(std::make_shared<FileBrowserScreen>(kSdMountPoint, "SD Card"));
}

void HomeScreen::open_usb_drive() {
    if (!usb_msc_is_mounted()) {
        esp_err_t err = usb_msc_mount(kUsbMountPoint, 0);
        if (err == ESP_ERR_NOT_FOUND) {
            show_mount_error("USB Drive", "No USB drive connected", ESP_OK);
            return;
        }
        if (err != ESP_OK) {
            show_mount_error("USB Drive", "Failed to mount USB drive", err);
            return;
        }
    }
    screen_manager.push(std::make_shared<FileBrowserScreen>(kUsbMountPoint, "USB Drive"));
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
