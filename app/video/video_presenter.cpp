/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "video_presenter.hpp"
#include "mjpeg_decoder.hpp"

#include <atomic>
#include <cstring>

#include "bsp.h"
#include "display_manager.hpp"
#include "driver/ppa.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "video_presenter";

static constexpr uint32_t kScaleSteps = 16;
static constexpr int kMaxFrameBuffers = 3;
static constexpr uint32_t kOverlayPeriodMs = 100;
static constexpr std::size_t kAlignment = 64;
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

static ppa_client_handle_t s_ppa;
static MjpegDecoder s_decoder;
static uint8_t *s_scratch;
static std::size_t s_scratch_capacity;

static lv_display_t *s_overlay;
static bsp_size_t s_panel;
static bsp_pixel_format_t s_format = BSP_PIXEL_FORMAT_RGB565;
static std::size_t s_bytes_per_pixel = 2;
static ppa_srm_color_mode_t s_color_mode = PPA_SRM_COLOR_MODE_RGB565;
static bsp_rotation_t s_rotation = BSP_ROTATION_0;
static int s_fb_count;
static int s_fb_index;
static std::atomic<bool> s_overlay_dirty{false};
static std::atomic<bool> s_repaint{false};
static bool s_have_picture;
static DecodedFrame s_last_frame;
static uint32_t s_last_width;
static uint32_t s_last_height;
static std::string s_error;
static float s_fps;
static int64_t s_last_us;

static std::size_t align_up(std::size_t value, std::size_t alignment) {
    return (value + alignment - 1) / alignment * alignment;
}

static bool swaps_axes(bsp_rotation_t rotation) {
    return rotation == BSP_ROTATION_90 || rotation == BSP_ROTATION_270;
}

static ppa_srm_rotation_angle_t ppa_rotation(bsp_rotation_t rotation) {
    switch (rotation) {
    case BSP_ROTATION_90:  return PPA_SRM_ROTATION_ANGLE_90;
    case BSP_ROTATION_180: return PPA_SRM_ROTATION_ANGLE_180;
    case BSP_ROTATION_270: return PPA_SRM_ROTATION_ANGLE_270;
    default:               return PPA_SRM_ROTATION_ANGLE_0;
    }
}

static bool color_mode_for(bsp_pixel_format_t format, ppa_srm_color_mode_t *out) {
    switch (format) {
    case BSP_PIXEL_FORMAT_RGB565: *out = PPA_SRM_COLOR_MODE_RGB565; return true;
    case BSP_PIXEL_FORMAT_RGB888: *out = PPA_SRM_COLOR_MODE_RGB888; return true;
    default: return false;
    }
}

static std::size_t framebuffer_bytes() {
    return (std::size_t)s_panel.width * s_panel.height * s_bytes_per_pixel;
}

static void set_error(const std::string &message) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_error = message;
    xSemaphoreGive(s_lock);
}

class ScratchAllocator : public FrameAllocator {
public:
    FrameBuffer lease(uint32_t pic_w, uint32_t pic_h) override {
        const std::size_t needed = align_up((std::size_t)pic_w * pic_h * s_bytes_per_pixel, kAlignment);
        if (!s_scratch || s_scratch_capacity < needed) {
            s_have_picture = false;
            heap_caps_free(s_scratch);
            s_scratch = (uint8_t *)heap_caps_aligned_alloc(
                kAlignment, needed, MALLOC_CAP_SPIRAM | MALLOC_CAP_CACHE_ALIGNED);
            if (!s_scratch) {
                s_scratch = (uint8_t *)heap_caps_aligned_alloc(kAlignment, needed, MALLOC_CAP_DEFAULT);
            }
            s_scratch_capacity = s_scratch ? needed : 0;
        }
        return { s_scratch, s_scratch_capacity };
    }
};

static ScratchAllocator s_scratch_allocator;

static void clear_framebuffer(int index) {
    void *framebuffer = bsp_display_get_frame_buffer(index);
    if (framebuffer) memset(framebuffer, 0, framebuffer_bytes());
}

static bool blit(const DecodedFrame &frame, int index) {
    void *framebuffer = bsp_display_get_frame_buffer(index);
    if (!framebuffer) return false;

    const bool swap = swaps_axes(s_rotation);
    const uint32_t fit_w = swap ? (uint32_t)s_panel.height : (uint32_t)s_panel.width;
    const uint32_t fit_h = swap ? (uint32_t)s_panel.width : (uint32_t)s_panel.height;

    uint32_t steps = kScaleSteps;
    while (steps > 1 && (frame.width * steps / kScaleSteps > fit_w ||
                         frame.height * steps / kScaleSteps > fit_h)) {
        steps--;
    }
    const uint32_t out_w = frame.width * steps / kScaleSteps;
    const uint32_t out_h = frame.height * steps / kScaleSteps;
    if (!out_w || !out_h || out_w > fit_w || out_h > fit_h) return false;

    const uint32_t panel_w = swap ? out_h : out_w;
    const uint32_t panel_h = swap ? out_w : out_h;
    const uint32_t offset_x = ((uint32_t)s_panel.width - panel_w) / 2;
    const uint32_t offset_y = ((uint32_t)s_panel.height - panel_h) / 2;

    if (panel_w != s_last_width || panel_h != s_last_height) {
        for (int i = 0; i < s_fb_count; i++) clear_framebuffer(i);
        s_last_width = panel_w;
        s_last_height = panel_h;
    }

    ppa_srm_oper_config_t op = {};
    op.in.buffer = frame.pixels;
    op.in.pic_w = frame.pic_w;
    op.in.pic_h = frame.pic_h;
    op.in.block_w = frame.width;
    op.in.block_h = frame.height;
    op.in.srm_cm = s_color_mode;
    op.out.buffer = framebuffer;
    op.out.buffer_size = framebuffer_bytes();
    op.out.pic_w = (uint32_t)s_panel.width;
    op.out.pic_h = (uint32_t)s_panel.height;
    op.out.block_offset_x = offset_x;
    op.out.block_offset_y = offset_y;
    op.out.srm_cm = s_color_mode;
    op.rotation_angle = ppa_rotation(s_rotation);
    op.scale_x = (float)steps / kScaleSteps;
    op.scale_y = (float)steps / kScaleSteps;
    op.mode = PPA_TRANS_MODE_BLOCKING;

    const esp_err_t err = ppa_do_scale_rotate_mirror(s_ppa, &op);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ppa srm: %s", esp_err_to_name(err));
        return false;
    }
    return true;
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

static void draw(const Job &job) {
    DecodedFrame frame;
    std::string failure;
    if (!s_decoder.decode(job.data, job.len, s_scratch_allocator, &frame, &failure)) {
        set_error(failure);
        return;
    }

    const int next = (s_fb_index + 1) % s_fb_count;
    if (!blit(frame, next)) return;
    s_last_frame = frame;
    s_have_picture = true;
    s_fb_index = next;
    set_error({});
    present(next);
    note_presented();
}

static void repaint() {
    const int next = (s_fb_index + 1) % s_fb_count;
    if (s_have_picture) {
        blit(s_last_frame, next);
    } else {
        clear_framebuffer(next);
    }
    s_fb_index = next;
    present(next);
}

static void worker(void *) {
    while (s_running.load()) {
        xSemaphoreTake(s_wake, pdMS_TO_TICKS(kOverlayPeriodMs));

        Job job = {};
        if (xQueueReceive(s_queue, &job, 0) == pdTRUE) {
            xSemaphoreTake(s_idle, 0);
            if (s_running.load()) draw(job);
            if (job.release) job.release(job.ctx);
            s_overlay_dirty.store(false);
            s_repaint.store(false);
            xSemaphoreGive(s_idle);
            continue;
        }
        if (!s_running.load()) break;
        if (s_repaint.exchange(false)) {
            s_overlay_dirty.store(false);
            repaint();
            continue;
        }
        if (s_overlay_dirty.exchange(false)) present(s_fb_index);
    }

    s_task = nullptr;
    xSemaphoreGive(s_done);
    vTaskDelete(nullptr);
}

static bool create_resources() {
    if (!s_ppa) {
        ppa_client_config_t config = {};
        config.oper_type = PPA_OPERATION_SRM;
        config.max_pending_trans_num = 1;
        const esp_err_t err = ppa_register_client(&config, &s_ppa);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ppa_register_client: %s", esp_err_to_name(err));
            set_error(std::string("video output unavailable: ") + esp_err_to_name(err));
            return false;
        }
    }
    std::string failure;
    if (!s_decoder.open(s_format, &failure)) {
        set_error(failure);
        return false;
    }
    return true;
}

bool video_presenter_begin(bsp_rotation_t rotation, lv_display_t *overlay) {
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
    s_format = bsp_display_get_pixel_format();
    if (!color_mode_for(s_format, &s_color_mode)) {
        set_error("unsupported panel pixel format for video");
        return false;
    }
    s_bytes_per_pixel = bsp_pixel_format_bytes(s_format);
    s_fb_count = 0;
    for (int i = 0; i < kMaxFrameBuffers; i++) {
        if (!bsp_display_get_frame_buffer(i)) break;
        s_fb_count++;
    }
    if (s_fb_count == 0) {
        set_error("no framebuffers for video output");
        return false;
    }
    if (!create_resources()) return false;

    s_overlay = overlay;
    s_rotation = rotation;
    s_fb_index = s_fb_count - 1;
    s_have_picture = false;
    s_last_frame = {};
    s_last_width = 0;
    s_last_height = 0;
    s_overlay_dirty.store(false);
    s_repaint.store(false);
    s_fps = 0.0f;
    s_last_us = 0;
    set_error({});
    for (int i = 0; i < s_fb_count; i++) clear_framebuffer(i);

    xSemaphoreTake(s_done, 0);
    s_running.store(true);
    if (xTaskCreatePinnedToCore(worker, "video_presenter", 4096, nullptr, 5, &s_task, 0) != pdPASS) {
        s_running.store(false);
        set_error("video output task creation failed");
        return false;
    }
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
    s_overlay = nullptr;
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
    if (s_idle && xSemaphoreTake(s_idle, pdMS_TO_TICKS(1000)) == pdTRUE) {
        xSemaphoreGive(s_idle);
    }
    while (xQueueReceive(s_queue, &job, 0) == pdTRUE) {
        if (job.release) job.release(job.ctx);
    }
}

void video_presenter_mark_overlay_dirty() {
    if (!s_running.load()) return;
    s_overlay_dirty.store(true);
    if (s_wake) xSemaphoreGive(s_wake);
}

void video_presenter_repaint() {
    if (!s_running.load()) return;
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
