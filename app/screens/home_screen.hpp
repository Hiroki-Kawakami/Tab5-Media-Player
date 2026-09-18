/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "screen_manager.hpp"
#include "widgets.hpp"

class HomeScreen : public NavigationScreen {
public:
    void build() override;

private:
    void open_sd_card();
    void open_usb_drive();
    void show_mount_error(const char *title, const char *message, esp_err_t err);
};
