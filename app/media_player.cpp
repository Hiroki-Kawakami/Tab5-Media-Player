/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "media_player.hpp"
#include "display_manager.hpp"
#include "lvgl.hpp"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "bench/h264_bench.hpp"
#include "playback/player.hpp"
#include "screens/home_screen.hpp"
#include "ui_orientation.hpp"

static const char *TAG = "media_player";

static constexpr std::size_t kMediaArenaBytes = 4 * 1024 * 1024;

alignas(64) static uint8_t s_shared_sram[kSharedSramBytes];
static lv_display_t *s_main;

static SharedSram shared_sram() {
    const std::size_t half = kSharedSramBytes / 2;
    return { s_shared_sram, kSharedSramBytes, { s_shared_sram, s_shared_sram + half }, half };
}

static esp_err_t display_init() {
    lvgl_port_cfg_t config = {
        .task_priority     = 4,
        .task_stack        = 8192,
        .task_affinity     = 1,
        .task_max_sleep_ms = 500,
        .task_stack_caps   = MALLOC_CAP_INTERNAL | MALLOC_CAP_DEFAULT,
        .timer_period_ms   = 5,
    };
    esp_err_t err = lvgl_port_init(&config);
    if (err != ESP_OK) return err;

    const SharedSram sram = shared_sram();
    DisplayManagerConfig display_config = {};
    display_config.render_mode = DisplayRenderMode::Partial;
    display_config.buffer.buffers[0] = sram.halves[0];
    display_config.buffer.buffers[1] = sram.halves[1];
    display_config.buffer.buffer_size = sram.half_bytes;
    return display_manager.create_display(display_config, &s_main);
}

SharedSram media_player_acquire_sram() {
    display_manager.set_visible(s_main, false);
    bsp_display_wait_draw();
    return shared_sram();
}

void media_player_release_sram() {
    display_manager.set_visible(s_main, true);
}

void app_entry() {
    bsp_config_t bsp_config = {};
    bsp_config.display.fb_num = 3;
    bsp_config.display.pixel_format = BSP_PIXEL_FORMAT_RGB888;
    bsp_config.dispatch.task_priority = 6;
    bsp_config.dispatch.task_affinity = 1;
    bsp_init(&bsp_config);

    esp_err_t err = display_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "display init: %s", esp_err_to_name(err));
        return;
    }
#ifdef ESP_PLATFORM
    ESP_LOGI(TAG, "internal heap free after display init: %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
#endif
    media_arena_t arena = {};
    arena.data = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        MB_ARENA_ALIGNMENT, kMediaArenaBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED));
    if (arena.data) {
        arena.size = kMediaArenaBytes;
    } else {
        ESP_LOGE(TAG, "no memory for the media arena");
    }
    player_start(arena);
    h264_bench_register();

    lv_async_call([] {
        ui_orientation_start(s_main);
        screen_manager.load(std::make_shared<HomeScreen>());
        bsp_display_set_brightness(80);
    });
}
