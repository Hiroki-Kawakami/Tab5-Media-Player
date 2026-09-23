/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "slideshow.hpp"
#include "slideshow_output.hpp"
#include "media/media_cache.hpp"
#include "media_player.hpp"
#include "playback/player.hpp"
#include "ui_orientation.hpp"
#include "bsp.h"
#include "display_manager.hpp"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lvgl.hpp"

static const char *TAG = "slideshow";

static constexpr uint32_t kBlackMs = 200;
static constexpr uint32_t kRetryMs = 1000;
static constexpr uint32_t kStackBytes = 6144;
static constexpr UBaseType_t kPriority = 3;
static constexpr BaseType_t kCore = 1;

static constexpr EventBits_t kStop = 1u << 0;
static constexpr EventBits_t kReady = 1u << 1;
static constexpr EventBits_t kTrack = 1u << 2;

namespace {

struct Session {
    std::vector<std::string> paths;
    ImageSize box;
    bool shown = false;
    std::size_t index = 0;
    SlideshowConfig config;
    SlideshowFinished on_finished;
};

struct Bgm {
    Playlist playlist;
    bool awaiting = false;
    std::size_t skips = 0;
};

}

static Session s_session;
static SlideshowOutput s_output;
static std::unique_ptr<Transition> s_change;
static bool s_running;
static EventGroupHandle_t s_events;
static uint32_t s_token;
static uint32_t s_idle_token;
static Bgm *s_bgm;

static void bgm_open() {
    s_bgm->awaiting = true;
    player_open(s_bgm->playlist.current().path);
    player_set_loop(s_bgm->playlist.size() == 1);
    player_play();
}

/* Notifications collapse, so the state is read rather than inferred. */
static void bgm_update() {
    if (!s_bgm) return;
    const PlayerStatus status = player_status();
    if (status.state == PlayerState::Failed) {
        if (++s_bgm->skips >= s_bgm->playlist.size()) return;
        s_bgm->playlist.step(1, RepeatMode::All);
        bgm_open();
    } else if (s_bgm->awaiting) {
        if (status.state != PlayerState::Playing) return;
        s_bgm->awaiting = false;
        s_bgm->skips = 0;
    } else if (status.state == PlayerState::Finished) {
        s_bgm->playlist.step(1, RepeatMode::All);
        bgm_open();
    }
}

static void bgm_changed() {
    xEventGroupSetBits(s_events, kTrack);
}

static void bgm_start() {
    if (!s_bgm) return;
    player_observe_state(bgm_changed);
    bgm_open();
}

static void bgm_stop() {
    if (!s_bgm) return;
    player_observe_state(nullptr);
    player_close();
    delete s_bgm;
    s_bgm = nullptr;
}

static EventBits_t wait(TickType_t ticks) {
    const TickType_t start = xTaskGetTickCount();
    for (;;) {
        const TickType_t elapsed = xTaskGetTickCount() - start;
        const TickType_t left = ticks == portMAX_DELAY ? portMAX_DELAY
                              : elapsed < ticks        ? ticks - elapsed
                                                       : 0;
        const EventBits_t bits =
            xEventGroupWaitBits(s_events, kStop | kReady | kTrack, pdTRUE, pdFALSE, left);
        if (bits & kTrack) bgm_update();
        if ((bits & (kStop | kReady)) || !(bits & kTrack)) return bits & (kStop | kReady);
    }
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

static bool fetch(std::size_t index, uint8_t *buffer, ImageSize *size, bool *stop) {
    const std::string &path = s_session.paths[index];
    for (;;) {
        if (media_cache_read_image(path, s_session.box, buffer, s_output.frame_bytes(), size)) {
            return true;
        }
        auto entry = media_cache_lookup(path);
        if (entry && (!entry->ok || entry->image_failed)) return false;
        const EventBits_t bits = wait(pdMS_TO_TICKS(kRetryMs));
        if (bits & kStop) {
            *stop = true;
            return false;
        }
        if (!bits) request(index);
    }
}

static bool stop_requested() {
    return xEventGroupGetBits(s_events) & kStop;
}


static bool play(Transition &transition) {
    const int64_t duration_us = transition.duration_us();
    const int64_t start_us = esp_timer_get_time();
    float previous = 0.0f;
    while (duration_us > 0) {
        if (stop_requested()) return false;
        const float t = (float)(esp_timer_get_time() - start_us) / (float)duration_us;
        if (t >= 1.0f) break;
        const float progress = transition_ease(s_session.config.curve, t);
        if (progress <= previous) continue;
        if (!transition.step(s_output, progress, previous)) break;
        previous = progress;
    }
    transition.finish(s_output);
    return true;
}

static void run() {
    bgm_start();
    const std::size_t count = s_session.paths.size();
    const int64_t interval_us = (int64_t)s_session.config.interval_ms * 1000;
    std::size_t target = s_session.index;
    const int black = s_output.least_recent(s_output.shown());
    s_output.fill_black(s_output.framebuffer(black));
    s_output.present(black);
    int64_t due_us = esp_timer_get_time() + (int64_t)kBlackMs * 1000;

    std::size_t misses = 0;
    while (!(s_session.shown && target == s_session.index) && misses < count) {
        request(target);
        uint8_t *buffer = s_change->pixels_buffer(s_output);
        ImageSize size;
        bool stop = false;
        const bool fetched = fetch(target, buffer, &size, &stop);
        if (stop) return;
        Placement to;
        if (fetched && s_output.place(buffer, size, &to) && s_change->prepare(s_output, to)) {
            if (!sleep_until(due_us) || !play(*s_change)) return;
            s_session.shown = true;
            s_session.index = target;
            misses = 0;
            due_us = esp_timer_get_time() + interval_us;
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
    const std::size_t index = s_session.index;
    s_session = {};
    if (on_finished) on_finished(index);
    media_player_release_sram();
    s_running = false;
}

static void task_main(void *) {
    run();
    bgm_stop();
    s_change.reset();
    media_cache_unobserve(s_token);
    media_cache_cancel(s_token);
    media_cache_cancel(s_idle_token);
    s_output.fill_black(s_output.framebuffer(0));
    s_output.present(0);
    s_output.close();
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

bool slideshow_start(std::vector<std::string> paths, std::size_t index, ImageSize box,
                     const SlideshowConfig &config, std::vector<PlaylistItem> bgm,
                     SlideshowFinished on_finished) {
    if (s_running || index >= paths.size() || !box.valid()) return false;
    if (!s_events) s_events = xEventGroupCreate();
    if (!s_token) {
        s_token = media_cache_token();
        s_idle_token = media_cache_token();
    }
    if (!s_events || !s_token) return false;
    if (!s_output.open(ui_orientation_current())) return false;
    s_change = transition_create(config.transition, config.direction, s_output);
    if (!s_change) {
        s_output.close();
        return false;
    }

    s_session = { std::move(paths), box, false, index, config, std::move(on_finished) };
    if (!bgm.empty()) s_bgm = new Bgm{ Playlist(std::move(bgm), 0) };
    xEventGroupClearBits(s_events, kStop | kReady);

    ui_orientation_set_listener([](bsp_rotation_t, void *) {}, nullptr);
    media_player_acquire_sram();
    media_cache_observe(s_token, image_ready);
    if (spawn() != pdPASS) {
        ESP_LOGE(TAG, "no task");
        delete s_bgm;
        s_bgm = nullptr;
        media_cache_unobserve(s_token);
        s_change.reset();
        s_output.close();
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
