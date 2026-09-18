/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "h264_bench.hpp"

#if defined(ESP_PLATFORM) && defined(H264_BENCH)

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "h264_dec.h"
#include "harness.h"
#include "lvgl.h"
#include "media_player.hpp"
#include "display_manager.hpp"
#include "video/h264_renderer.hpp"
#include "video/video_presenter.hpp"
#include "video/h264_threads.hpp"

static const char *TAG = "H264";

extern const uint8_t bench_clip_start[] asm("_binary_bench_h264_start");
extern const uint8_t bench_clip_end[] asm("_binary_bench_h264_end");

static constexpr uint32_t kBenchStackBytes = 8192;

struct BenchArgs {
    int loops;
    bool hash;
    bool threads;
    bool show;
    bool present;
    int fps;
};

static std::atomic<bool> s_busy{false};

struct AccessUnit {
    uint32_t offset;
    uint32_t len;
};

static uint32_t cycles_now() {
    return (uint32_t)esp_cpu_get_cycle_count();
}

static void *alloc_psram(void *, std::size_t bytes) {
    return heap_caps_aligned_alloc(64, bytes, MALLOC_CAP_SPIRAM);
}

static void free_psram(void *, void *ptr) {
    heap_caps_free(ptr);
}

static std::vector<AccessUnit> split(const uint8_t *data, std::size_t size) {
    std::vector<AccessUnit> units;
    std::size_t au = SIZE_MAX;
    bool has_vcl = false;
    for (std::size_t i = 0; i + 3 < size; i++) {
        if (data[i] || data[i + 1] || data[i + 2] != 1) continue;
        const uint8_t header = data[i + 3];
        const uint8_t type = header & 0x1F;
        const bool vcl = type == 1 || type == 5;
        const bool first = vcl && i + 4 < size && (data[i + 4] & 0x80);
        if (au != SIZE_MAX && has_vcl && (first || (type >= 6 && type <= 9))) {
            units.push_back({ (uint32_t)au, (uint32_t)(i - au) });
            au = SIZE_MAX;
            has_vcl = false;
        }
        if (au == SIZE_MAX) au = i;
        if (vcl) has_vcl = true;
        i += 2;
    }
    if (au != SIZE_MAX) units.push_back({ (uint32_t)au, (uint32_t)(size - au) });
    return units;
}

static uint32_t fnv1a(const uint8_t *data, std::size_t len) {
    uint32_t h = 2166136261u;
    for (std::size_t i = 0; i < len; i++) h = (h ^ data[i]) * 16777619u;
    return h;
}

static bool fit(bsp_size_t source, int index, RenderTarget *target) {
    const bsp_size_t panel = bsp_display_get_size();
    const uint32_t fit_w = (uint32_t)panel.height;
    const uint32_t fit_h = (uint32_t)panel.width;
    uint32_t n = std::min(fit_w * kScaleDenominator / (uint32_t)source.width,
                          fit_h * kScaleDenominator / (uint32_t)source.height);
    if (!n) return false;
    const int out_w = (int)((uint32_t)source.width * n / kScaleDenominator);
    const int out_h = (int)((uint32_t)source.height * n / kScaleDenominator);
    target->framebuffer = bsp_display_get_frame_buffer(index);
    if (!target->framebuffer) return false;
    target->framebuffer_bytes = (std::size_t)panel.width * panel.height *
                                bsp_pixel_format_bytes(bsp_display_get_pixel_format());
    target->panel = panel;
    target->source = source;
    target->rotation = BSP_ROTATION_90;
    target->scale_n = n;
    target->rect = { { (panel.width - out_h) / 2, (panel.height - out_w) / 2 }, { out_h, out_w } };
    return true;
}

static void run_show(BenchArgs *args, const uint8_t *clip, const std::vector<AccessUnit> &units,
                     const SharedSram &sram) {
    H264Renderer renderer;
    TrackInfo track;
    track.codec = CodecId::H264;
    std::string error;
    if (!renderer.open(sram, bsp_display_get_pixel_format(), track, &error)) {
        ESP_LOGE(TAG, "renderer: %s", error.c_str());
        return;
    }
    int fb = 0;
    for (int loop = 0; loop < args->loops; loop++) {
        int shown = 0;
        int64_t decode_us = 0;
        int64_t draw_us = 0;
        const int64_t start = esp_timer_get_time();
        renderer.restart();
        for (const AccessUnit &au : units) {
            VideoFrame frame;
            const int64_t t0 = esp_timer_get_time();
            const DecodeResult r = renderer.decode(clip + au.offset, au.len, nullptr, nullptr, true,
                                                   0, &frame, &error);
            int64_t due = 0;
            const int64_t t1 = esp_timer_get_time();
            decode_us += t1 - t0;
            if (r == DecodeResult::Failed || !renderer.take(&frame, &due)) continue;
            const int next = (fb + 1) % 3;
            RenderTarget target;
            if (!fit(frame.size, next, &target)) {
                renderer.drop(&frame);
                continue;
            }
            if (!renderer.draw(&frame, target, &error)) continue;
            display_manager.present(next);
            fb = next;
            draw_us += esp_timer_get_time() - t1;
            shown++;
        }
        const double secs = (esp_timer_get_time() - start) / 1e6;
        ESP_LOGI(TAG, "show loop %d: %d frames, %.1f fps end-to-end, decode %.2f ms, ppa+present %.2f ms",
                 loop, shown, shown / secs, shown ? decode_us / 1000.0 / shown : 0,
                 shown ? draw_us / 1000.0 / shown : 0);
    }
    renderer.close();
}

static void run_present(BenchArgs *args, const uint8_t *clip, const std::vector<AccessUnit> &units,
                        const SharedSram &sram) {
    if (!video_presenter_begin(sram, BSP_ROTATION_90)) {
        ESP_LOGE(TAG, "presenter: %s", video_presenter_error().c_str());
        return;
    }
    TrackInfo track;
    track.codec = CodecId::H264;
    std::string error;
    if (!video_presenter_open_stream(track, &error)) {
        ESP_LOGE(TAG, "stream: %s", error.c_str());
        video_presenter_end();
        return;
    }
    const int64_t interval = 1000000 / args->fps;
    const int64_t lead = 120000;
    for (int loop = 0; loop < args->loops; loop++) {
        const int64_t start = esp_timer_get_time() + 200000;
        int hidden = 0;
        int index = 0;
        for (const AccessUnit &au : units) {
            const int64_t due = start + index * interval;
            index++;
            int64_t now = esp_timer_get_time();
            if (now < due - lead) {
                vTaskDelay(pdMS_TO_TICKS((due - lead - now) / 1000) + 1);
                now = esp_timer_get_time();
            }
            const bool late = now > due + interval;
            if (late) hidden++;
            video_presenter_submit(clip + au.offset, au.len, nullptr, nullptr, !late, due);
        }
        vTaskDelay(pdMS_TO_TICKS(400));
        const double secs = (esp_timer_get_time() - start) / 1e6;
        ESP_LOGI(TAG, "present loop %d: target %d fps, %d frames, %d decoded hidden, presenter %.1f fps, %.1fs",
                 loop, args->fps, index, hidden, video_presenter_fps(), secs);
        video_presenter_flush();
    }
    video_presenter_end();
}

static void run(BenchArgs *args) {
    const std::size_t size = (std::size_t)(bench_clip_end - bench_clip_start);
    auto *clip = static_cast<uint8_t *>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM));
    if (!clip) {
        ESP_LOGE(TAG, "no memory for the clip");
        return;
    }
    memcpy(clip, bench_clip_start, size);
    const std::vector<AccessUnit> units = split(clip, size);

    lv_lock();
    const SharedSram sram = media_player_acquire_sram();
    lv_unlock();

    if (args->show || args->present) {
        if (args->present) run_present(args, clip, units, sram);
        else run_show(args, clip, units, sram);
        lv_lock();
        media_player_release_sram();
        lv_unlock();
        heap_caps_free(clip);
        return;
    }

    h264_dec_config_t config = {};
    config.alloc = alloc_psram;
    config.free = free_psram;
    config.work = static_cast<uint8_t *>(sram.base);
    config.work_bytes = sram.bytes;
    config.max_mbs = 3600;
    config.max_side = 1280;
    config.held_pictures = 2;
    config.clock = cycles_now;
    config.threads = args->threads ? h264_threads() : nullptr;
    h264_dec_t *dec = h264_dec_create(&config);
    if (!dec) {
        ESP_LOGE(TAG, "decoder create failed");
    } else {
        ESP_LOGI(TAG, "clip %u bytes, %u access units, internal free %u", (unsigned)size,
                 (unsigned)units.size(), (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        for (int loop = 0; loop < args->loops; loop++) {
            int frames = 0;
            int errors = 0;
            int64_t total_us = 0;
            int64_t worst_us = 0;
            uint64_t total_cycles = 0;
            h264_dec_flush(dec);
            uint64_t profile[H264_PROF_COUNT];
            h264_dec_take_profile(dec, profile);
            for (const AccessUnit &au : units) {
                const uint32_t c0 = cycles_now();
                const int64_t t0 = esp_timer_get_time();
                const h264_dec_result_t r = h264_dec_decode(dec, clip + au.offset, au.len, 0, 0);
                const int64_t t1 = esp_timer_get_time();
                total_cycles += (uint32_t)(cycles_now() - c0);
                total_us += t1 - t0;
                if (t1 - t0 > worst_us) worst_us = t1 - t0;
                if (r != H264_DEC_OK && r != H264_DEC_NO_PICTURE) errors++;
                h264_dec_picture_t pic = {};
                while (h264_dec_output(dec, &pic)) {
                    if (pic.concealed) errors++;
                    if (args->hash && loop == 0) {
                        printf("[H264V] f=%d h=%08" PRIx32 "\n", frames,
                               fnv1a(pic.packed, pic.packed_bytes));
                    }
                    h264_dec_release(dec, pic.id);
                    frames++;
                }
            }
            h264_dec_drain(dec);
            {
                h264_dec_picture_t pic = {};
                while (h264_dec_output(dec, &pic)) {
                    if (pic.concealed) errors++;
                    if (args->hash && loop == 0) {
                        printf("[H264V] f=%d h=%08" PRIx32 "\n", frames,
                               fnv1a(pic.packed, pic.packed_bytes));
                    }
                    h264_dec_release(dec, pic.id);
                    frames++;
                }
            }
            const double ms = frames ? total_us / 1000.0 / frames : 0;
            ESP_LOGI(TAG, "loop %d: %d frames %d errors, %.2f ms/frame (worst %.2f), %.1f fps, %.2f Mcyc/frame",
                     loop, frames, errors, ms, worst_us / 1000.0, ms > 0 ? 1000.0 / ms : 0,
                     frames ? total_cycles / 1e6 / frames : 0);
            if (h264_dec_take_profile(dec, profile) && profile[H264_PROF_TOTAL]) {
                static const char *const kNames[] = { "mc_luma", "(fetch)", "window", "mc_chroma", "intra", "cavlc", "idct",
                                                      "deblock", "pack", "flush", "wait" };
                const double total = (double)profile[H264_PROF_TOTAL];
                uint64_t accounted = 0;
                char line[320];
                int n = 0;
                for (int i = 0; i < H264_PROF_TOTAL; i++) {
                    if (i != H264_PROF_MC_FETCH) accounted += profile[i];
                    n += snprintf(line + n, sizeof(line) - n, "%s %.1f%% ", kNames[i],
                                  100.0 * profile[i] / total);
                }
                snprintf(line + n, sizeof(line) - n, "rest %.1f%%",
                         100.0 * (total - (double)accounted) / total);
                ESP_LOGI(TAG, "share: %s", line);
            }
        }
        h264_dec_destroy(dec);
    }

    lv_lock();
    media_player_release_sram();
    lv_unlock();
    heap_caps_free(clip);
}

static void bench_task(void *arg) {
    auto *args = static_cast<BenchArgs *>(arg);
    run(args);
    ESP_LOGI(TAG, "bench done");
    delete args;
    s_busy.store(false);
    vTaskDeleteWithCaps(nullptr);
}

static bool bench_command(int argc, const char *const *argv, void *) {
    const int loops = argc > 1 ? atoi(argv[1]) : 3;
    if (loops <= 0 || s_busy.exchange(true)) return false;
    bool hash = false;
    bool threads = true;
    bool show = false;
    bool present = false;
    int fps = 30;
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "hash") == 0) hash = true;
        if (strcmp(argv[i], "single") == 0) threads = false;
        if (strcmp(argv[i], "show") == 0) show = true;
        if (strcmp(argv[i], "present") == 0) present = true;
        if (strncmp(argv[i], "fps=", 4) == 0) fps = atoi(argv[i] + 4);
    }
    if (fps <= 0) return false;
    auto *args = new BenchArgs{ loops, hash, threads, show, present, fps };
    if (h264_create_task(bench_task, "h264_bench", kBenchStackBytes, args, 5, 0, nullptr) != pdPASS) {
        delete args;
        s_busy.store(false);
        return false;
    }
    return true;
}

void h264_bench_register() {
    harness_register("h264bench", bench_command, nullptr);
}

#else

void h264_bench_register() {}

#endif
