/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "home_screen.hpp"

#include <string>
#include "bsp.h"
#include "file_browser_screen.hpp"

static constexpr const char *kSdMountPoint = "/sdcard";

void HomeScreen::build() {
    createNavigation("Tab5-Media-Player");

    auto sd_card = lv_button_create(contents_, LV_BUTTON_STYLE_PRIMARY);
    lv_obj_set_width(sd_card, LV_PCT(100));
    lv_button_set_text(sd_card, LV_SYMBOL_SD_CARD "  SD Card");
    lv_obj_add_event_fn(sd_card, LV_EVENT_CLICKED, [this](lv_event_t *) { open_sd_card(); });
}

void HomeScreen::open_sd_card() {
    if (!bsp_sd_is_mounted()) {
        bsp_sd_mount_config_t config = {};
        esp_err_t err = bsp_sd_mount(kSdMountPoint, &config);
        if (err != ESP_OK) {
            std::string message = std::string("Failed to mount SD card\n") + esp_err_to_name(err);
            auto modal = lv_modal_open(root_);
            lv_modal_title_create(modal, "SD Card");
            lv_modal_message_create(modal, message.c_str());
            lv_modal_button_create(modal, "Close", LV_MODAL_BUTTON_TYPE_PRIMARY,
                                   [modal](lv_event_t *) { lv_modal_close(modal); });
            return;
        }
    }
    screen_manager.push(std::make_shared<FileBrowserScreen>(kSdMountPoint, "SD Card"));
}
