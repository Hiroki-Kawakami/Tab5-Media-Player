#include <cstdio>
#include "bsp_tab5.h"

extern "C" void app_main() {
    bsp_tab5_config_t bsp_config = {};
    bsp_config.display.fb_num = 1;
    bsp_tab5_init(&bsp_config);
    bsp_tab5_display_set_brightness(100);
    uint16_t *fb = (uint16_t*)bsp_tab5_display_get_frame_buffer(0);
    for (int i = 0; i < 1280 * 720; i++) {
        fb[i] = 0xffff;
    }
    bsp_tab5_display_flush(0);
}
