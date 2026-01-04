#pragma once
#include "lvgl.h"

struct Resources {
    struct {
        const lv_image_dsc_t *play_circle;
        const lv_image_dsc_t *pause_circle;
        const lv_image_dsc_t *volume_off;
        const lv_image_dsc_t *volume_mute;
        const lv_image_dsc_t *volume_down;
        const lv_image_dsc_t *volume_up;
        const lv_image_dsc_t *brightness_low;
        const lv_image_dsc_t *brightness_mid;
        const lv_image_dsc_t *brightness_high;
    } icon;
};
extern const struct Resources R;
