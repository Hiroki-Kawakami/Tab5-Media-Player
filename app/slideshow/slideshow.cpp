/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "slideshow.hpp"
#include "media/media_cache.hpp"
#include "media_player.hpp"
#include "ui_orientation.hpp"
#include "bsp.h"
#include "display_manager.hpp"
#include "driver/ppa.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lvgl.hpp"
#ifdef ESP_PLATFORM
#include "esp_cache.h"
#endif

#include <cstring>

static const char *TAG = "slideshow";

static constexpr uint32_t kBlackMs = 200;
static constexpr uint32_t kRetryMs = 1000;
static constexpr uint32_t kStackBytes = 6144;
static constexpr UBaseType_t kPriority = 3;
static constexpr BaseType_t kCore = 1;

static constexpr EventBits_t kStop = 1u << 0;
static constexpr EventBits_t kReady = 1u << 1;

namespace {

struct Session {
    std::vector<std::string> paths;
    bsp_rotation_t rotation = BSP_ROTATION_0;
    ImageBox box;
    std::shared_ptr<const ImagePixels> shown;
    std::size_t index = 0;
    int64_t interval_us = 0;
    SlideshowFinished on_finished;
};

}

static Session s_session;
static bool s_running;
static EventGroupHandle_t s_events;
static ppa_client_handle_t s_srm;
static uint32_t s_token;
static uint32_t s_idle_token;
static bsp_size_t s_panel;
static uint8_t s_bytes_per_pixel;
static ppa_srm_color_mode_t s_panel_mode;
static int s_fb_index;

static bool ppa_mode(bsp_pixel_format_t format, ppa_srm_color_mode_t *out) {
    switch (format) {
    case BSP_PIXEL_FORMAT_RGB565: *out = PPA_SRM_COLOR_MODE_RGB565; return true;
    case BSP_PIXEL_FORMAT_RGB888: *out = PPA_SRM_COLOR_MODE_RGB888; return true;
    default: return false;
    }
}

static ppa_srm_rotation_angle_t ppa_rotation(bsp_rotation_t rotation) {
    switch (rotation) {
    case BSP_ROTATION_90: return PPA_SRM_ROTATION_ANGLE_90;
    case BSP_ROTATION_180: return PPA_SRM_ROTATION_ANGLE_180;
    case BSP_ROTATION_270: return PPA_SRM_ROTATION_ANGLE_270;
    default: return PPA_SRM_ROTATION_ANGLE_0;
    }
}

static void fill_black(uint8_t *framebuffer, bsp_rect_t area) {
    if (area.size.width <= 0 || area.size.height <= 0) return;
    const std::size_t stride = (std::size_t)s_panel.width * s_bytes_per_pixel;
    const std::size_t row_bytes = (std::size_t)area.size.width * s_bytes_per_pixel;
    const std::size_t first = (std::size_t)area.origin.y * stride +
                              (std::size_t)area.origin.x * s_bytes_per_pixel;
    for (int row = 0; row < area.size.height; row++) {
        memset(framebuffer + first + (std::size_t)row * stride, 0, row_bytes);
    }
#ifdef ESP_PLATFORM
    const std::size_t span = (std::size_t)(area.size.height - 1) * stride + row_bytes;
    esp_cache_msync(framebuffer + first, span,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#endif
}

static bool present(std::shared_ptr<const ImagePixels> pixels) {
    const bool swap = s_session.rotation == BSP_ROTATION_90 ||
                      s_session.rotation == BSP_ROTATION_270;
    const int width = swap ? pixels->height : pixels->width;
    const int height = swap ? pixels->width : pixels->height;
    if (!pixels->data || !width || width > s_panel.width || !height || height > s_panel.height) {
        return false;
    }

    const int index = s_fb_index == 1 ? 2 : 1;
    auto *framebuffer = static_cast<uint8_t *>(bsp_display_get_frame_buffer(index));
    if (!framebuffer) return false;

    const int left = (s_panel.width - width) / 2;
    const int top = (s_panel.height - height) / 2;
    fill_black(framebuffer, { { 0, 0 }, { s_panel.width, top } });
    fill_black(framebuffer, { { 0, top + height }, { s_panel.width, s_panel.height - top - height } });
    fill_black(framebuffer, { { 0, top }, { left, height } });
    fill_black(framebuffer, { { left + width, top }, { s_panel.width - left - width, height } });

    ppa_srm_oper_config_t op = {};
    op.in.buffer = pixels->data;
    op.in.pic_w = pixels->width;
    op.in.pic_h = pixels->height;
    op.in.block_w = pixels->width;
    op.in.block_h = pixels->height;
    op.in.srm_cm = pixels->rgb888 ? PPA_SRM_COLOR_MODE_RGB888 : PPA_SRM_COLOR_MODE_RGB565;
    op.out.buffer = framebuffer;
    op.out.buffer_size = (uint32_t)s_panel.width * s_panel.height * s_bytes_per_pixel;
    op.out.pic_w = s_panel.width;
    op.out.pic_h = s_panel.height;
    op.out.block_offset_x = left;
    op.out.block_offset_y = top;
    op.out.srm_cm = s_panel_mode;
    op.rotation_angle = ppa_rotation(s_session.rotation);
    op.scale_x = 1.0f;
    op.scale_y = 1.0f;
    op.mode = PPA_TRANS_MODE_BLOCKING;
    const esp_err_t err = ppa_do_scale_rotate_mirror(s_srm, &op);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "blit: %s", esp_err_to_name(err));
        return false;
    }
    display_manager.present(index);
    s_fb_index = index;
    s_session.shown = std::move(pixels);
    return true;
}

static EventBits_t wait(TickType_t ticks) {
    return xEventGroupWaitBits(s_events, kStop | kReady, pdTRUE, pdFALSE, ticks) &
           (kStop | kReady);
}

static bool sleep_until(int64_t due_us) {
    for (;;) {
        const int64_t left_us = due_us - esp_timer_get_time();
        if (left_us <= 0) return true;
        if (wait(pdMS_TO_TICKS((uint32_t)((left_us + 999) / 1000))) & kStop) return false;
    }
}

static std::size_t next_of(std::size_t index) {
    return (index + 1) % s_session.paths.size();
}

static void request(std::size_t index) {
    constexpr uint8_t want = MetaWantInfo | MetaWantImage;
    media_cache_request(s_session.paths[index], want, s_session.box, MetaPriority::Blocking,
                        s_token);
    media_cache_idle_cancel();
    const std::size_t after = next_of(index);
    if (after == index) return;
    media_cache_request(s_session.paths[after], want, s_session.box, MetaPriority::Idle,
                        s_idle_token);
}

static std::shared_ptr<const ImagePixels> fetch(std::size_t index, bool *stop) {
    const std::string &path = s_session.paths[index];
    for (;;) {
        if (auto pixels = media_cache_image(path, s_session.box)) return pixels;
        auto entry = media_cache_lookup(path);
        if (entry && (!entry->ok || entry->image_failed)) return nullptr;
        const EventBits_t bits = wait(pdMS_TO_TICKS(kRetryMs));
        if (bits & kStop) {
            *stop = true;
            return nullptr;
        }
        if (!bits) request(index);
    }
}

static void run() {
    const std::size_t count = s_session.paths.size();
    std::size_t target = s_session.index;
    int64_t due_us = esp_timer_get_time();
    if (s_session.shown && present(std::move(s_session.shown))) {
        target = next_of(target);
        due_us += s_session.interval_us;
    }

    std::size_t misses = 0;
    while (!(s_session.shown && target == s_session.index) && misses < count) {
        request(target);
        if (!sleep_until(due_us)) return;
        bool stop = false;
        auto pixels = fetch(target, &stop);
        if (stop) return;
        if (pixels && present(std::move(pixels))) {
            s_session.index = target;
            misses = 0;
            due_us = esp_timer_get_time() + s_session.interval_us;
        } else {
            misses++;
        }
        target = next_of(target);
    }
    while (!(wait(portMAX_DELAY) & kStop)) {
    }
}

static void finish() {
    display_manager.set_outside_touch_callback(nullptr);
    ui_orientation_set_listener(nullptr, nullptr);
    SlideshowFinished on_finished = std::move(s_session.on_finished);
    auto shown = std::move(s_session.shown);
    const std::size_t index = s_session.index;
    s_session = {};
    if (on_finished) on_finished(index, std::move(shown));
    media_player_release_sram();
    s_running = false;
}

static void task_main(void *) {
    run();
    media_cache_unobserve(s_token);
    media_cache_cancel(s_token);
    media_cache_cancel(s_idle_token);
    ppa_unregister_client(s_srm);
    s_srm = nullptr;

    if (auto *framebuffer = static_cast<uint8_t *>(bsp_display_get_frame_buffer(0))) {
        fill_black(framebuffer, { { 0, 0 }, s_panel });
        display_manager.present(0);
    }
    vTaskDelay(pdMS_TO_TICKS(kBlackMs));

    lv_lock();
    lv_async_call([] { finish(); });
    lv_unlock();
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

static void image_ready(const std::string &) {
    xEventGroupSetBits(s_events, kReady);
}

static void touched(const bsp_touch_point_t *, int count, void *) {
    if (count > 0) xEventGroupSetBits(s_events, kStop);
}

static BaseType_t spawn() {
#ifdef ESP_PLATFORM
    return xTaskCreatePinnedToCoreWithCaps(task_main, "slideshow", kStackBytes, nullptr, kPriority,
                                           nullptr, kCore, MALLOC_CAP_SPIRAM);
#else
    return xTaskCreatePinnedToCore(task_main, "slideshow", kStackBytes, nullptr, kPriority,
                                   nullptr, kCore);
#endif
}

bool slideshow_start(std::vector<std::string> paths, std::size_t index, ImageBox box,
                     std::shared_ptr<const ImagePixels> first, uint32_t interval_ms,
                     SlideshowFinished on_finished) {
    if (s_running || index >= paths.size() || !box.valid()) return false;
    if (!s_events) s_events = xEventGroupCreate();
    if (!s_token) {
        s_token = media_cache_token();
        s_idle_token = media_cache_token();
    }
    if (!s_events || !s_token) return false;

    const bsp_pixel_format_t format = bsp_display_get_pixel_format();
    if (!ppa_mode(format, &s_panel_mode) || !bsp_display_get_frame_buffer(1) ||
        !bsp_display_get_frame_buffer(2)) {
        ESP_LOGE(TAG, "no framebuffers to draw into");
        return false;
    }
    ppa_client_config_t client = {};
    client.oper_type = PPA_OPERATION_SRM;
    client.max_pending_trans_num = 1;
    const esp_err_t err = ppa_register_client(&client, &s_srm);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no ppa client: %s", esp_err_to_name(err));
        s_srm = nullptr;
        return false;
    }

    s_panel = bsp_display_get_size();
    s_bytes_per_pixel = bsp_pixel_format_bytes(format);
    s_fb_index = 0;
    s_session = { std::move(paths), ui_orientation_current(), box, std::move(first), index,
                  (int64_t)interval_ms * 1000, std::move(on_finished) };
    xEventGroupClearBits(s_events, kStop | kReady);

    ui_orientation_set_listener([](bsp_rotation_t, void *) {}, nullptr);
    media_player_acquire_sram();
    media_cache_observe(s_token, image_ready);
    if (spawn() != pdPASS) {
        ESP_LOGE(TAG, "no task");
        media_cache_unobserve(s_token);
        ppa_unregister_client(s_srm);
        s_srm = nullptr;
        s_session = {};
        ui_orientation_set_listener(nullptr, nullptr);
        media_player_release_sram();
        return false;
    }
    display_manager.set_outside_touch_callback(touched);
    s_running = true;
    return true;
}

void slideshow_stop() {
    if (s_running) xEventGroupSetBits(s_events, kStop);
}
