#include "lvgl.h"
#include <SDL2/SDL.h>
#include <stdio.h>
#include <unistd.h>
#include "mediaplayer.hpp"

lv_display_t * sdl_hal_init(int32_t w, int32_t h) {
    lv_group_set_default(lv_group_create());

    lv_display_t *disp = lv_sdl_window_create(w, h);
    lv_indev_t *mouse = lv_sdl_mouse_create();
    lv_indev_set_group(mouse, lv_group_get_default());
    lv_indev_set_display(mouse, disp);
    lv_display_set_default(disp);

    return disp;
}

extern "C" int main(void) {
    lv_init();
    sdl_hal_init(720, 1280);
    mediaplayer_app();

    while (1) {
        /* Periodically call the lv_task handler.
        * It could be done in a timer interrupt or an OS task too.*/
        uint32_t sleep_time_ms = lv_timer_handler();
        if (sleep_time_ms == LV_NO_TIMER_READY){
            sleep_time_ms =  LV_DEF_REFR_PERIOD;
        }
        usleep(sleep_time_ms * 1000);
    }
    return 0;
}
