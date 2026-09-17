/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "ui_orientation.hpp"
#include "bsp.h"
#include "display_manager.hpp"
#include "esp_log.h"
#include "lvgl.hpp"

static const char *TAG = "ui_orientation";

static lv_display_t *s_main;
static bsp_rotation_t s_current = BSP_ROTATION_0;
static bsp_rotation_t s_main_rotation = BSP_ROTATION_0;
static OrientationListener s_listener;
static void *s_listener_arg;

static void rotate_main() {
    if (!s_main || s_listener || s_main_rotation == s_current) return;
    const esp_err_t err = display_manager.set_rotation(s_main, s_current);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "set_rotation: %s", esp_err_to_name(err));
        return;
    }
    s_main_rotation = s_current;
}

static void orientation_changed(bsp_imu_orientation_t orientation, void *) {
    if (orientation < BSP_IMU_ORIENTATION_ROTATION_0 ||
        orientation > BSP_IMU_ORIENTATION_ROTATION_270) {
        return;
    }
    const auto rotation = static_cast<bsp_rotation_t>(orientation);
    lv_lock();
    lv_async_call([rotation] {
        if (rotation == s_current) return;
        s_current = rotation;
        if (s_listener) {
            s_listener(rotation, s_listener_arg);
        } else {
            rotate_main();
        }
    });
    lv_unlock();
}

void ui_orientation_start(lv_display_t *main_display) {
    s_main = main_display;
    bsp_imu_set_orientation_cb(orientation_changed, nullptr);
    const esp_err_t err = bsp_imu_set_orientation_enabled(true);
    if (err != ESP_OK) ESP_LOGW(TAG, "orientation tracking: %s", esp_err_to_name(err));
}

bsp_rotation_t ui_orientation_current() {
    return s_current;
}

void ui_orientation_set_listener(OrientationListener listener, void *arg) {
    s_listener = listener;
    s_listener_arg = arg;
    rotate_main();
}
