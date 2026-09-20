/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "media_player.hpp"
#include "lvgl.hpp"
#include "nvs_flash.h"

#include <cstdlib>

extern "C" int main(void) {
    if (const char *nvs_path = std::getenv("SIMULATOR_NVS_PATH")) nvs_flash_sim_set_path(nvs_path);
    app_entry();
    lvgl_sim_loop();
    return 0;
}
