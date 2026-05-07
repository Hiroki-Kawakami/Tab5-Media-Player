#include <cstdio>
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "bsp_tab5.h"
#include "mediaplayer.hpp"

static const char *TAG = "main";

// MARK: LVGL
static void lvgl_setup() {
    lvgl_port_cfg_t config = {
        .task_priority = 4,
        .task_stack = 7168,
        .task_affinity = 0,
        .task_max_sleep_ms = 500,
        .task_stack_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms = 5,
    };
    esp_err_t err = lvgl_port_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start LVGL: %s", esp_err_to_name(err));
        assert(0);
    }
}

void gui_init() {
    lvgl_setup();

    auto fb = bsp_tab5_display_get_frame_buffer(0);
    lv_display_t *disp = lv_display_create(720, 1280);
    lv_display_set_buffers(disp, fb, NULL, 720 * 1280 * 2, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(disp, [](lv_display_t *disp, const lv_area_t *area, uint8_t *px_map){
        bsp_tab5_display_flush(0);
        lv_display_flush_ready(disp);
    });
    bsp_tab5_display_set_brightness(80);

    lv_indev_t *indev = lv_indev_create();
    lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
    lv_indev_set_user_data(indev, NULL);
    lv_indev_set_read_cb(indev, [](lv_indev_t *indev, lv_indev_data_t *data){
        esp_lcd_touch_point_data_t touch;
        int touch_num = bsp_tab5_touch_read(&touch, 1);
        if (touch_num > 0) {
            data->state = LV_INDEV_STATE_PRESSED;
            data->point.x = touch.x;
            data->point.y = touch.y;
        } else {
            data->state = LV_INDEV_STATE_RELEASED;
        }
    });
}

extern "C" void app_main() {
    bsp_tab5_config_t bsp_config = {};
    bsp_config.display.fb_num = 1;
    bsp_tab5_init(&bsp_config);
    gui_init();

    mediaplayer_app();
}
