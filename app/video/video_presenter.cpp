/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "video_presenter.hpp"
#include "h264_renderer.hpp"
#include "video_threads.hpp"
#include "mjpeg_renderer.hpp"
#include "mpeg2_renderer.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>

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
static constexpr uint32_t kWakePeriodMs = 100;
static constexpr float kFpsSmoothing = 0.25f;
static constexpr uint32_t kSubmitTimeoutMs = 2000;
static constexpr uint32_t kStopTimeoutMs = 2000;
static constexpr uint32_t kStackBytes = 6144;
static constexpr uint32_t kDecodeStackBytes = 8192;
static constexpr UBaseType_t kDecodePriority = 2;
static constexpr int kReadyFrames = 2;
static constexpr float kDrawSmoothing = 0.125f;
static constexpr int64_t kLateDropUs = 8000;
static constexpr int64_t kDecoderBusyUs = 500000;
static constexpr uint32_t kDecoderRestMs = 20;

struct Job {
    const uint8_t *data;
    std::size_t len;
    VideoPresenterRelease release;
    void *ctx;
    bool present;
    int64_t due_us;
};

struct Ready {
    VideoFrame frame;
    int64_t due_us;
};

static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_wake;
static SemaphoreHandle_t s_idle;
static SemaphoreHandle_t s_done;
static SemaphoreHandle_t s_clip_done;
static QueueHandle_t s_queue;
static QueueHandle_t s_decode_queue;
static QueueHandle_t s_ready;
static SemaphoreHandle_t s_decode_idle;
static SemaphoreHandle_t s_decode_done;
static TaskHandle_t s_task;
static std::atomic<bool> s_running{false};
static std::atomic<bool> s_pipelined{false};
static std::atomic<bool> s_flushing{false};
static float s_draw_us;
static bool s_have_next;
static Ready s_next;

static std::unique_ptr<VideoRenderer> s_renderer;
static SharedSram s_sram;
static bsp_pixel_format_t s_format;

static bsp_size_t s_panel;
static std::size_t s_bytes_per_pixel = 2;
static bsp_rotation_t s_rotation = BSP_ROTATION_0;
static std::atomic<bsp_rotation_t> s_requested_rotation{BSP_ROTATION_0};
static bsp_rotation_t s_source_rotation = BSP_ROTATION_0;
static std::atomic<bsp_rotation_t> s_requested_source_rotation{BSP_ROTATION_0};
static int s_fb_count;
static int s_fb_index;
static std::atomic<bool> s_repaint{false};
static std::atomic<bool> s_clear_all{false};
static uint32_t s_clear_pending;
static bsp_rect_t s_rect;
static bsp_rect_t s_clip;
static bool s_shared;
static bsp_rect_t s_requested_clip;
static std::atomic<uint32_t> s_clip_request{0};
static uint32_t s_clip_seen;
static bool s_clip_signal;
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

static bsp_rect_t intersect(const bsp_rect_t &a, const bsp_rect_t &b) {
    const int x0 = std::max(a.origin.x, b.origin.x);
    const int y0 = std::max(a.origin.y, b.origin.y);
    const int x1 = std::min(a.origin.x + a.size.width, b.origin.x + b.size.width);
    const int y1 = std::min(a.origin.y + a.size.height, b.origin.y + b.size.height);
    if (x1 <= x0 || y1 <= y0) return {};
    return { { x0, y0 }, { x1 - x0, y1 - y0 } };
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

static void clear_outside(int index, const RenderTarget &target) {
    auto *framebuffer = (uint8_t *)bsp_display_get_frame_buffer(index);
    if (!framebuffer) return;
    const bsp_rect_t &rect = target.rect;
    const int right = rect.origin.x + rect.size.width;
    const int bottom = rect.origin.y + rect.size.height;
    fill_black(framebuffer, intersect(s_clip, { { 0, 0 }, { s_panel.width, rect.origin.y } }));
    fill_black(framebuffer, intersect(s_clip, { { 0, bottom }, { s_panel.width, s_panel.height - bottom } }));
    fill_black(framebuffer, intersect(s_clip, { { 0, rect.origin.y }, { rect.origin.x, rect.size.height } }));
    fill_black(framebuffer,
               intersect(s_clip, { { right, rect.origin.y }, { s_panel.width - right, rect.size.height } }));

    const bsp_rect_t visible = intersect(s_clip, rect);
    if (!visible.size.width) return;
    const int band = std::min<int>(2 * ((target.scale_n + kScaleDenominator - 1) / kScaleDenominator) + 1,
                                   std::min(visible.size.width, visible.size.height));
    const int visible_right = visible.origin.x + visible.size.width;
    const int visible_bottom = visible.origin.y + visible.size.height;
    if (visible.origin.y > rect.origin.y) {
        fill_black(framebuffer, { visible.origin, { visible.size.width, band } });
    }
    if (visible_bottom < bottom) {
        fill_black(framebuffer, { { visible.origin.x, visible_bottom - band }, { visible.size.width, band } });
    }
    if (visible.origin.x > rect.origin.x) {
        fill_black(framebuffer, { visible.origin, { band, visible.size.height } });
    }
    if (visible_right < right) {
        fill_black(framebuffer, { { visible_right - band, visible.origin.y }, { band, visible.size.height } });
    }
}

static void clear_framebuffer(int index) {
    auto *framebuffer = (uint8_t *)bsp_display_get_frame_buffer(index);
    if (framebuffer) fill_black(framebuffer, s_clip);
}

static int next_framebuffer() {
    return s_shared ? 0 : (s_fb_index + 1) % s_fb_count;
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
    target->clip = s_clip;
    return true;
}

static void prepare(int index, const RenderTarget &target) {
    if (!same_rect(target.rect, s_rect)) {
        s_rect = target.rect;
        s_clear_pending = all_framebuffers();
    }
    const uint32_t bit = 1u << index;
    if (s_clear_pending & bit) {
        clear_outside(index, target);
        s_clear_pending &= ~bit;
    }
}

static void present(int index) {
    const bool switched = index != s_fb_index;
    s_fb_index = index;
    if (!s_shared || switched) display_manager.present(index);
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

static void apply_clip(const bsp_rect_t &clip) {
    if (same_rect(clip, s_clip)) return;
    s_clip = clip;
    s_shared = !same_rect(clip, { { 0, 0 }, s_panel });
    s_clear_pending = all_framebuffers();
    s_repaint.store(true);
}

static void consume_requests() {
    const uint32_t clip_request = s_clip_request.load();
    if (clip_request != s_clip_seen) {
        s_clip_seen = clip_request;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        const bsp_rect_t clip = s_requested_clip;
        xSemaphoreGive(s_lock);
        apply_clip(clip);
        s_clip_signal = true;
    }
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

static void release_job(const Job &job) {
    if (job.release) job.release(job.ctx);
}

static void show_frame(VideoFrame *frame) {
    std::string failure;
    const bsp_size_t source = frame->size;
    const int next = next_framebuffer();
    RenderTarget target;
    if (!place(source, next, &target)) {
        s_renderer->drop(frame);
        set_error("video does not fit the panel");
        return;
    }
    prepare(next, target);
    const int64_t start = esp_timer_get_time();
    if (!s_renderer->draw(frame, target, &failure)) {
        set_error(failure);
        return;
    }
    const float took = (float)(esp_timer_get_time() - start);
    s_draw_us = s_draw_us > 0.0f ? s_draw_us + (took - s_draw_us) * kDrawSmoothing : took;
    s_source = source;
    set_error({});
    present(next);
    note_presented();
}

static void draw(const Job &job) {
    if (!s_renderer) {
        release_job(job);
        return;
    }
    std::string failure;
    VideoFrame frame;
    const DecodeResult result = s_renderer->decode(job.data, job.len, job.release, job.ctx,
                                                   job.present, job.due_us, &frame, &failure);
    if (result == DecodeResult::Failed) {
        set_error(failure);
        return;
    }
    if (result == DecodeResult::Hidden) return;
    show_frame(&frame);
}

static void repaint() {
    const int next = next_framebuffer();
    RenderTarget target;
    if (s_renderer && s_renderer->has_picture() && place(s_source, next, &target)) {
        prepare(next, target);
        std::string failure;
        if (!s_renderer->draw(nullptr, target, &failure)) {
            set_error(failure);
            return;
        }
    } else {
        clear_framebuffer(next);
        s_clear_pending &= ~(1u << next);
    }
    present(next);
}

static void drop_next() {
    if (!s_have_next) return;
    s_have_next = false;
    if (s_renderer) s_renderer->drop(&s_next.frame);
}

static TickType_t pending_wait() {
    if (!s_have_next && xQueuePeek(s_ready, &s_next, 0) == pdTRUE) {
        xQueueReceive(s_ready, &s_next, 0);
        s_have_next = true;
    }
    if (!s_have_next || !s_next.due_us) return pdMS_TO_TICKS(kWakePeriodMs);
    const int64_t wait = s_next.due_us - (int64_t)s_draw_us - esp_timer_get_time();
    if (wait <= 0) return 0;
    const TickType_t ticks = pdMS_TO_TICKS(wait / 1000);
    return std::min<TickType_t>(ticks ? ticks : 1, pdMS_TO_TICKS(kWakePeriodMs));
}

static bool take_due(Ready *out) {
    if (!s_have_next) return false;
    const int64_t now = esp_timer_get_time();
    if (s_next.due_us && s_next.due_us - (int64_t)s_draw_us > now) return false;
    Ready newer;
    while (xQueuePeek(s_ready, &newer, 0) == pdTRUE &&
           (!newer.due_us || newer.due_us - kLateDropUs <= now)) {
        xQueueReceive(s_ready, &newer, 0);
        drop_next();
        s_next = newer;
        s_have_next = true;
    }
    *out = s_next;
    s_have_next = false;
    return true;
}

static void worker(void *) {
    while (s_running.load()) {
        const TickType_t wait = s_pipelined.load() ? pending_wait() : pdMS_TO_TICKS(kWakePeriodMs);
        if (wait) xSemaphoreTake(s_wake, wait);

        xSemaphoreTake(s_idle, portMAX_DELAY);
        Job job = {};
        bool has_job = xQueueReceive(s_queue, &job, 0) == pdTRUE;
        Ready ready = {};
        const bool has_ready = !has_job && s_pipelined.load() && (pending_wait(), take_due(&ready));
        if (!has_job && !has_ready && !s_running.load()) {
            xSemaphoreGive(s_idle);
            break;
        }
        consume_requests();
        if (has_job || has_ready) {
            if (!s_running.load()) {
                if (has_job) release_job(job);
                if (has_ready && s_renderer) s_renderer->drop(&ready.frame);
            } else if (has_job) {
                draw(job);
            } else {
                show_frame(&ready.frame);
            }
            s_repaint.store(false);
        } else if (s_repaint.exchange(false)) {
            repaint();
        }
        if (s_clip_signal) {
            s_clip_signal = false;
            xSemaphoreGive(s_clip_done);
        }
        xSemaphoreGive(s_idle);
    }

    s_task = nullptr;
    xSemaphoreGive(s_done);
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

static void push_ready(const Ready &ready) {
    while (xQueueSend(s_ready, &ready, pdMS_TO_TICKS(kWakePeriodMs)) != pdTRUE) {
        if (s_flushing.load() || !s_running.load()) {
            Ready copy = ready;
            if (s_renderer) s_renderer->drop(&copy.frame);
            return;
        }
    }
    xSemaphoreGive(s_wake);
}

static void take_ready() {
    Ready ready = {};
    while (s_renderer->take(&ready.frame, &ready.due_us)) {
        push_ready(ready);
        ready = {};
    }
}

static void decoder(void *) {
    int64_t rested_us = esp_timer_get_time();
    while (s_running.load()) {
        if (esp_timer_get_time() - rested_us > kDecoderBusyUs) {
            vTaskDelay(pdMS_TO_TICKS(kDecoderRestMs));
            rested_us = esp_timer_get_time();
        }
        Job job = {};
        if (xQueueReceive(s_decode_queue, &job, 0) != pdTRUE) {
            if (xQueueReceive(s_decode_queue, &job, pdMS_TO_TICKS(kWakePeriodMs)) != pdTRUE) {
                rested_us = esp_timer_get_time();
                continue;
            }
            rested_us = esp_timer_get_time();
        }
        xSemaphoreTake(s_decode_idle, portMAX_DELAY);
        if (!s_running.load() || s_flushing.load() || !s_renderer) {
            release_job(job);
        } else if (!job.data) {
            s_renderer->drain();
            take_ready();
        } else {
            std::string failure;
            Ready ready = {};
            ready.due_us = job.due_us;
            const DecodeResult result = s_renderer->decode(job.data, job.len, job.release, job.ctx,
                                                           job.present, job.due_us, &ready.frame,
                                                           &failure);
            if (result == DecodeResult::Failed) {
                set_error(failure);
            } else if (result == DecodeResult::Ready) {
                push_ready(ready);
            }
            take_ready();
        }
        xSemaphoreGive(s_decode_idle);
    }
    xSemaphoreGive(s_decode_done);
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

static void hand_back_framebuffer() {
    auto *framebuffer = (uint8_t *)bsp_display_get_frame_buffer(0);
    if (!framebuffer) return;
    fill_black(framebuffer, { { 0, 0 }, s_panel });
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
    if (!s_clip_done) s_clip_done = xSemaphoreCreateBinary();
    if (!s_decode_done) s_decode_done = xSemaphoreCreateBinary();
    if (!s_decode_idle) {
        s_decode_idle = xSemaphoreCreateBinary();
        if (s_decode_idle) xSemaphoreGive(s_decode_idle);
    }
    if (!s_queue) s_queue = xQueueCreate(1, sizeof(Job));
    if (!s_decode_queue) s_decode_queue = xQueueCreate(1, sizeof(Job));
    if (!s_ready) s_ready = xQueueCreate(kReadyFrames, sizeof(Ready));
    if (!s_lock || !s_wake || !s_idle || !s_done || !s_clip_done || !s_queue || !s_decode_done ||
        !s_decode_idle || !s_decode_queue || !s_ready) {
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
    s_sram = sram;
    s_format = format;
    s_renderer.reset();

    s_rotation = rotation;
    s_requested_rotation.store(rotation);
    s_source_rotation = BSP_ROTATION_0;
    s_requested_source_rotation.store(BSP_ROTATION_0);
    s_fb_index = s_fb_count - 1;
    s_rect = {};
    s_clip = { { 0, 0 }, s_panel };
    s_shared = false;
    s_clip_seen = s_clip_request.load();
    s_clip_signal = false;
    s_source = {};
    s_clear_pending = all_framebuffers();
    s_clear_all.store(false);
    s_repaint.store(true);
    s_fps = 0.0f;
    s_last_us = 0;
    s_draw_us = 0.0f;
    s_have_next = false;
    s_pipelined.store(false);
    s_flushing.store(false);
    set_error({});

    xSemaphoreTake(s_done, 0);
    xSemaphoreTake(s_decode_done, 0);
    s_running.store(true);
    if (video_create_task(worker, "video_presenter", kStackBytes, nullptr, 6, 0, &s_task) != pdPASS) {
        s_running.store(false);
        set_error("video output task creation failed");
        return false;
    }
    if (video_create_task(decoder, "video_decoder", kDecodeStackBytes, nullptr, kDecodePriority, 0, nullptr) !=
        pdPASS) {
        s_running.store(false);
        xSemaphoreGive(s_wake);
        xSemaphoreTake(s_done, pdMS_TO_TICKS(kStopTimeoutMs));
        set_error("video decoder task creation failed");
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
    if (xSemaphoreTake(s_decode_done, pdMS_TO_TICKS(kStopTimeoutMs)) != pdTRUE) {
        ESP_LOGE(TAG, "decoder did not stop");
    }
    video_presenter_flush();
    xSemaphoreTake(s_idle, portMAX_DELAY);
    xSemaphoreTake(s_decode_idle, portMAX_DELAY);
    s_renderer.reset();
    xSemaphoreGive(s_decode_idle);
    xSemaphoreGive(s_idle);
    hand_back_framebuffer();
}

bool video_presenter_open_stream(const TrackInfo &track, std::string *error) {
    if (!s_running.load() || !s_idle) {
        *error = "video output is not running";
        return false;
    }
    std::unique_ptr<VideoRenderer> renderer;
    switch (track.codec) {
    case CodecId::Mjpeg: renderer = std::make_unique<MjpegRenderer>(); break;
    case CodecId::H264: renderer = std::make_unique<H264Renderer>(); break;
    case CodecId::Mpeg2: renderer = std::make_unique<Mpeg2Renderer>(); break;
    default:
        *error = "unsupported video codec";
        return false;
    }
    xSemaphoreTake(s_idle, portMAX_DELAY);
    xSemaphoreTake(s_decode_idle, portMAX_DELAY);
    s_renderer.reset();
    s_pipelined.store(false);
    const bool ok = renderer->open(s_sram, s_format, track, error);
    if (ok) {
        s_pipelined.store(renderer->pipelined());
        s_renderer = std::move(renderer);
    }
    xSemaphoreGive(s_decode_idle);
    xSemaphoreGive(s_idle);
    return ok;
}

bool video_presenter_pipelined() {
    return s_pipelined.load();
}

bool video_presenter_submit(const uint8_t *data, std::size_t len,
                            VideoPresenterRelease release, void *ctx, bool present,
                            int64_t due_us) {
    if (!s_running.load() || !s_queue) return false;
    Job job = { data, len, release, ctx, present, due_us };
    const QueueHandle_t queue = s_pipelined.load() ? s_decode_queue : s_queue;
    if (xQueueSend(queue, &job, pdMS_TO_TICKS(kSubmitTimeoutMs)) != pdTRUE) return false;
    xSemaphoreGive(s_wake);
    return true;
}

static void drain_jobs(QueueHandle_t queue) {
    Job job = {};
    while (xQueueReceive(queue, &job, 0) == pdTRUE) release_job(job);
}

static void drain_ready() {
    Ready ready = {};
    while (xQueueReceive(s_ready, &ready, 0) == pdTRUE) {
        if (s_renderer) s_renderer->drop(&ready.frame);
    }
}

void video_presenter_drain() {
    if (!s_running.load() || !s_decode_queue || !s_pipelined.load()) return;
    Job job = {};
    if (xQueueSend(s_decode_queue, &job, pdMS_TO_TICKS(kSubmitTimeoutMs)) != pdTRUE) return;
    xSemaphoreGive(s_wake);
}

void video_presenter_flush() {
    if (!s_queue) return;
    s_flushing.store(true);
    drain_jobs(s_decode_queue);
    drain_jobs(s_queue);
    drain_ready();
    const bool decoder_idle = xSemaphoreTake(s_decode_idle, pdMS_TO_TICKS(kStopTimeoutMs)) == pdTRUE;
    if (!decoder_idle) ESP_LOGE(TAG, "flush: decode in flight did not finish");
    if (!s_idle || xSemaphoreTake(s_idle, pdMS_TO_TICKS(kStopTimeoutMs)) != pdTRUE) {
        ESP_LOGE(TAG, "flush: frame in flight did not finish");
        if (decoder_idle) xSemaphoreGive(s_decode_idle);
        s_flushing.store(false);
        return;
    }
    drain_jobs(s_decode_queue);
    drain_jobs(s_queue);
    drain_ready();
    drop_next();
    if (s_renderer) {
        s_renderer->discard();
        s_renderer->restart();
    }
    xSemaphoreGive(s_idle);
    if (decoder_idle) xSemaphoreGive(s_decode_idle);
    s_flushing.store(false);
}

void video_presenter_set_ui_insets(const VideoInsets &insets) {
    if (!s_lock) return;
    const int left = std::clamp(insets.left, 0, s_panel.width);
    const int top = std::clamp(insets.top, 0, s_panel.height);
    const int right = std::clamp(s_panel.width - insets.right, left, s_panel.width);
    const int bottom = std::clamp(s_panel.height - insets.bottom, top, s_panel.height);
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_requested_clip = { { left, top }, { right - left, bottom - top } };
    xSemaphoreGive(s_lock);
    if (!s_running.load()) return;
    xSemaphoreTake(s_clip_done, 0);
    s_clip_request.fetch_add(1);
    xSemaphoreGive(s_wake);
    if (xSemaphoreTake(s_clip_done, pdMS_TO_TICKS(kStopTimeoutMs)) != pdTRUE) {
        ESP_LOGE(TAG, "ui insets were not applied");
    }
}

void video_presenter_set_rotation(bsp_rotation_t rotation) {
    s_requested_rotation.store(rotation);
    if (s_wake) xSemaphoreGive(s_wake);
}

void video_presenter_set_source_rotation(bsp_rotation_t rotation) {
    s_requested_source_rotation.store(rotation);
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
