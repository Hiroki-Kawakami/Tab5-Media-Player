/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "video_presenter.hpp"
#include "mjpeg_renderer.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "bsp.h"
#include "display_manager.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#ifdef ESP_PLATFORM
#include "esp_cache.h"
#endif

static const char *TAG = "video_presenter";

static constexpr uint32_t kMaxScaleN = 255 * kScaleDenominator + kScaleDenominator - 1;
static constexpr int kMaxFrameBuffers = 3;
static constexpr uint32_t kOverlayPeriodMs = 100;
static constexpr float kFpsSmoothing = 0.25f;
static constexpr uint32_t kSubmitTimeoutMs = 2000;
static constexpr uint32_t kStopTimeoutMs = 2000;

struct Job {
    const uint8_t *data;
    std::size_t len;
    VideoPresenterRelease release;
    void *ctx;
};

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_wake;
static SemaphoreHandle_t s_idle;
static SemaphoreHandle_t s_done;
static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static std::atomic<bool> s_running{false};

static MjpegRenderer s_renderer;

static lv_display_t *s_overlay;
static bsp_size_t s_panel;
static std::size_t s_bytes_per_pixel = 2;
static bsp_rotation_t s_rotation = BSP_ROTATION_0;
static std::atomic<bsp_rotation_t> s_requested_rotation{BSP_ROTATION_0};
static bsp_rotation_t s_source_rotation = BSP_ROTATION_0;
static std::atomic<bsp_rotation_t> s_requested_source_rotation{BSP_ROTATION_0};
static int s_fb_count;
static int s_fb_index;
static std::atomic<bool> s_overlay_dirty{false};
static std::atomic<bool> s_repaint{false};
static std::atomic<bool> s_clear_all{false};
static uint32_t s_clear_pending;
static bsp_rect_t s_rect;
static bsp_size_t s_source;
static std::string s_error;
static float s_fps;
static int64_t s_last_us;

static bool swaps_axes(bsp_rotation_t rotation) {
    return rotation == BSP_ROTATION_90 || rotation == BSP_ROTATION_270;
}

static bsp_rotation_t output_rotation() {
    return static_cast<bsp_rotation_t>((s_rotation + s_source_rotation) % 4);
}

static bool same_rect(const bsp_rect_t &a, const bsp_rect_t &b) {
    return a.origin.x == b.origin.x && a.origin.y == b.origin.y &&
           a.size.width == b.size.width && a.size.height == b.size.height;
}

static uint32_t all_framebuffers() {
    return (1u << s_fb_count) - 1;
}

static std::size_t framebuffer_bytes() {
    return (std::size_t)s_panel.width * s_panel.height * s_bytes_per_pixel;
}

static void set_error(const std::string &message) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_error = message;
    xSemaphoreGive(s_lock);
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

static void clear_outside(int index, const bsp_rect_t &rect) {
    auto *framebuffer = (uint8_t *)bsp_display_get_frame_buffer(index);
    if (!framebuffer) return;
    const int right = rect.origin.x + rect.size.width;
    const int bottom = rect.origin.y + rect.size.height;
    fill_black(framebuffer, { { 0, 0 }, { s_panel.width, rect.origin.y } });
    fill_black(framebuffer, { { 0, bottom }, { s_panel.width, s_panel.height - bottom } });
    fill_black(framebuffer, { { 0, rect.origin.y }, { rect.origin.x, rect.size.height } });
    fill_black(framebuffer, { { right, rect.origin.y }, { s_panel.width - right, rect.size.height } });
}

static void clear_framebuffer(int index) {
    auto *framebuffer = (uint8_t *)bsp_display_get_frame_buffer(index);
    if (framebuffer) fill_black(framebuffer, { { 0, 0 }, s_panel });
}

static bool place(bsp_size_t source, int index, RenderTarget *target) {
    const bsp_rotation_t rotation = output_rotation();
    const bool swap = swaps_axes(rotation);
    const uint32_t fit_w = (uint32_t)(swap ? s_panel.height : s_panel.width);
    const uint32_t fit_h = (uint32_t)(swap ? s_panel.width : s_panel.height);
    const uint32_t src_w = (uint32_t)source.width;
    const uint32_t src_h = (uint32_t)source.height;
    if (!src_w || !src_h) return false;

    uint32_t n = std::min(fit_w * kScaleDenominator / src_w, fit_h * kScaleDenominator / src_h);
    n = std::clamp<uint32_t>(n, 1, kMaxScaleN);
    const uint32_t out_w = src_w * n / kScaleDenominator;
    const uint32_t out_h = src_h * n / kScaleDenominator;
    if (!out_w || !out_h || out_w > fit_w || out_h > fit_h) return false;

    const int panel_w = (int)(swap ? out_h : out_w);
    const int panel_h = (int)(swap ? out_w : out_h);
    target->framebuffer = bsp_display_get_frame_buffer(index);
    if (!target->framebuffer) return false;
    target->framebuffer_bytes = framebuffer_bytes();
    target->panel = s_panel;
    target->source = source;
    target->rotation = rotation;
    target->scale_n = n;
    target->rect = { { (s_panel.width - panel_w) / 2, (s_panel.height - panel_h) / 2 },
                     { panel_w, panel_h } };
    return true;
}

static void prepare(int index, const bsp_rect_t &rect) {
    if (!same_rect(rect, s_rect)) {
        s_rect = rect;
        s_clear_pending = all_framebuffers();
    }
    const uint32_t bit = 1u << index;
    if (s_clear_pending & bit) {
        clear_outside(index, rect);
        s_clear_pending &= ~bit;
    }
}

static void present(int index) {
    if (s_overlay) display_manager.compose(s_overlay, index);
    display_manager.present(index);
}

static void note_presented() {
    const int64_t now = esp_timer_get_time();
    if (s_last_us) {
        const float seconds = (float)(now - s_last_us) / 1000000.0f;
        if (seconds > 0.0f) {
            const float instant = 1.0f / seconds;
            xSemaphoreTake(s_lock, portMAX_DELAY);
            s_fps = s_fps > 0.0f ? s_fps + (instant - s_fps) * kFpsSmoothing : instant;
            xSemaphoreGive(s_lock);
        }
    }
    s_last_us = now;
}

static void consume_requests() {
    const bsp_rotation_t rotation = s_requested_rotation.load();
    const bsp_rotation_t source_rotation = s_requested_source_rotation.load();
    if (rotation != s_rotation || source_rotation != s_source_rotation) {
        s_rotation = rotation;
        s_source_rotation = source_rotation;
        s_clear_all.store(true);
        s_repaint.store(true);
    }
    if (s_clear_all.exchange(false)) s_clear_pending = all_framebuffers();
}

static void draw(const Job &job) {
    std::string failure;
    bsp_size_t source = {};
    if (!s_renderer.probe(job.data, job.len, &source, &failure)) {
        set_error(failure);
        if (job.release) job.release(job.ctx);
        return;
    }

    const int next = (s_fb_index + 1) % s_fb_count;
    RenderTarget target;
    if (!place(source, next, &target)) {
        set_error("video does not fit the panel");
        if (job.release) job.release(job.ctx);
        return;
    }
    prepare(next, target.rect);
    if (!s_renderer.render(job.data, job.len, job.release, job.ctx, target, &failure)) {
        set_error(failure);
        return;
    }
    s_source = source;
    s_fb_index = next;
    set_error({});
    present(next);
    note_presented();
}

static void repaint() {
    const int next = (s_fb_index + 1) % s_fb_count;
    RenderTarget target;
    if (s_renderer.has_picture() && place(s_source, next, &target)) {
        prepare(next, target.rect);
        std::string failure;
        if (!s_renderer.rerender(target, &failure)) {
            set_error(failure);
            return;
        }
    } else {
        clear_framebuffer(next);
        s_clear_pending &= ~(1u << next);
    }
    s_fb_index = next;
    present(next);
}

static void worker(void *) {
    while (s_running.load()) {
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(kOverlayPeriodMs));

        Job job = {};
        const bool has_job = xQueueReceive(s_queue, &job, 0) == pdTRUE;
        if (!has_job && !s_running.load()) break;

        xSemaphoreTake(s_idle, portMAX_DELAY);
        consume_requests();
        if (has_job) {
            if (s_running.load()) {
                draw(job);
            } else if (job.release) {
                job.release(job.ctx);
            }
            s_overlay_dirty.store(false);
            s_repaint.store(false);
        } else if (s_repaint.exchange(false)) {
            s_overlay_dirty.store(false);
            repaint();
        } else if (s_overlay_dirty.exchange(false)) {
            present(s_fb_index);
        }
        xSemaphoreGive(s_idle);
    }

    s_task = nullptr;
    xSemaphoreGive(s_done);
    vTaskDelete(nullptr);
}

static void hand_back_framebuffer() {
    if (!bsp_display_get_frame_buffer(0)) return;
    clear_framebuffer(0);
    display_manager.present(0);
}

bool video_presenter_begin(const SharedSram &sram, bsp_rotation_t rotation) {
    if (s_running.load()) return true;

    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_wake) s_wake = xSemaphoreCreateBinary();
    if (!s_idle) {
        s_idle = xSemaphoreCreateBinary();
        if (s_idle) xSemaphoreGive(s_idle);
    }
    if (!s_done) s_done = xSemaphoreCreateBinary();
    if (!s_queue) s_queue = xQueueCreate(1, sizeof(Job));
    if (!s_lock || !s_wake || !s_idle || !s_done || !s_queue) {
        ESP_LOGE(TAG, "allocation failed");
        return false;
    }

    s_panel = bsp_display_get_size();
    const bsp_pixel_format_t format = bsp_display_get_pixel_format();
    s_bytes_per_pixel = bsp_pixel_format_bytes(format);
    s_fb_count = 0;
    for (int i = 0; i < kMaxFrameBuffers; i++) {
        if (!bsp_display_get_frame_buffer(i)) break;
        s_fb_count++;
    }
    if (s_fb_count == 0) {
        set_error("no framebuffers for video output");
        return false;
    }
    std::string failure;
    if (!s_renderer.open(sram, format, &failure)) {
        set_error(failure);
        return false;
    }

    s_overlay = nullptr;
    s_rotation = rotation;
    s_requested_rotation.store(rotation);
    s_source_rotation = BSP_ROTATION_0;
    s_requested_source_rotation.store(BSP_ROTATION_0);
    s_fb_index = s_fb_count - 1;
    s_rect = {};
    s_source = {};
    s_clear_pending = all_framebuffers();
    s_clear_all.store(false);
    s_overlay_dirty.store(false);
    s_repaint.store(true);
    s_fps = 0.0f;
    s_last_us = 0;
    set_error({});

    xSemaphoreTake(s_done, 0);
    s_running.store(true);
    if (xTaskCreatePinnedToCore(worker, "video_presenter", 4096, nullptr, 5, &s_task, 0) != pdPASS) {
        s_running.store(false);
        s_renderer.close();
        set_error("video output task creation failed");
        return false;
    }
    xSemaphoreGive(s_wake);
    ESP_LOGI(TAG, "%d framebuffers, panel %dx%d", s_fb_count, s_panel.width, s_panel.height);
    return true;
}

void video_presenter_end() {
    if (!s_running.exchange(false)) return;
    if (s_wake) xSemaphoreGive(s_wake);
    if (xSemaphoreTake(s_done, pdMS_TO_TICKS(kStopTimeoutMs)) != pdTRUE) {
        ESP_LOGE(TAG, "presenter did not stop");
    }
    video_presenter_flush();
    xSemaphoreTake(s_idle, portMAX_DELAY);
    s_renderer.close();
    s_overlay = nullptr;
    xSemaphoreGive(s_idle);
    hand_back_framebuffer();
}

bool video_presenter_submit(const uint8_t *data, std::size_t len,
                            VideoPresenterRelease release, void *ctx) {
    if (!s_running.load() || !s_queue) return false;
    Job job = { data, len, release, ctx };
    if (xQueueSend(s_queue, &job, pdMS_TO_TICKS(kSubmitTimeoutMs)) != pdTRUE) return false;
    xSemaphoreGive(s_wake);
    return true;
}

void video_presenter_flush() {
    if (!s_queue) return;
    Job job = {};
    while (xQueueReceive(s_queue, &job, 0) == pdTRUE) {
        if (job.release) job.release(job.ctx);
    }
    if (!s_idle || xSemaphoreTake(s_idle, pdMS_TO_TICKS(kStopTimeoutMs)) != pdTRUE) {
        ESP_LOGE(TAG, "flush: frame in flight did not finish");
        return;
    }
    while (xQueueReceive(s_queue, &job, 0) == pdTRUE) {
        if (job.release) job.release(job.ctx);
    }
    s_renderer.discard();
    xSemaphoreGive(s_idle);
}

void video_presenter_set_overlay(lv_display_t *overlay) {
    if (s_idle) xSemaphoreTake(s_idle, portMAX_DELAY);
    s_overlay = overlay;
    s_clear_all.store(true);
    s_repaint.store(true);
    if (s_idle) xSemaphoreGive(s_idle);
    if (s_wake) xSemaphoreGive(s_wake);
}

void video_presenter_set_rotation(bsp_rotation_t rotation) {
    s_requested_rotation.store(rotation);
    if (s_wake) xSemaphoreGive(s_wake);
}

void video_presenter_set_source_rotation(bsp_rotation_t rotation) {
    s_requested_source_rotation.store(rotation);
    if (s_wake) xSemaphoreGive(s_wake);
}

void video_presenter_mark_overlay_dirty() {
    if (!s_running.load()) return;
    s_overlay_dirty.store(true);
    if (s_wake) xSemaphoreGive(s_wake);
}

void video_presenter_repaint() {
    if (!s_running.load()) return;
    s_clear_all.store(true);
    s_repaint.store(true);
    if (s_wake) xSemaphoreGive(s_wake);
}

float video_presenter_fps() {
    if (!s_lock) return 0.0f;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const float fps = s_fps;
    xSemaphoreGive(s_lock);
    return fps;
}

std::string video_presenter_error() {
    if (!s_lock) return {};
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const std::string error = s_error;
    xSemaphoreGive(s_lock);
    return error;
}
