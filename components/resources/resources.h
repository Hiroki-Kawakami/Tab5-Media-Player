#pragma once
#include "lvgl.h"

struct Resources {
    struct {
        const lv_image_dsc_t *play_circle;
        const lv_image_dsc_t *pause_circle;
    } icon;
};
extern const struct Resources R;
