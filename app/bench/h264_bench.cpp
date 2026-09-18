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

#include "esp_async_memcpy.h"
#include "esp_cache.h"
#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "hal/cache_ll.h"
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

static uint32_t s_ticks_per_us;

static void psram_report(const char *name, std::vector<uint32_t> &v, size_t bytes) {
    std::sort(v.begin(), v.end());
    const uint32_t m = v[v.size() / 2];
    const double us = (double)m / s_ticks_per_us;
    ESP_LOGI(TAG, "psram %s: %u cyc median, %.2f us, %.1f MB/s", name, m, us, bytes / us);
}

static void psram_invalidate(const volatile uint8_t *base, size_t bytes) {
    esp_cache_msync((void *)base, bytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

static bool psram_wait_preload(uint32_t max_cycles) {
    const uint32_t t0 = cycles_now();
    while (!Cache_L2_Cache_Preload_Done()) {
        if (cycles_now() - t0 > max_cycles) return false;
    }
    return true;
}

static uint32_t psram_read_rows(const volatile uint8_t *base, int rows, size_t stride, size_t row_bytes) {
    uint32_t sum = 0;
    for (int r = 0; r < rows; r++) {
        const volatile uint8_t *p = base + (size_t)r * stride;
        for (size_t b = 0; b < row_bytes; b++) sum += p[b];
    }
    return sum;
}

static uint32_t a1_independent(const volatile uint8_t *base, int rows, size_t stride) {
    uint8_t v[32];
    for (int r = 0; r < rows; r++) v[r] = base[(size_t)r * stride];
    uint32_t sum = 0;
    for (int r = 0; r < rows; r++) sum += v[r];
    return sum;
}

static uint32_t a1_dependent(const volatile uint8_t *base, int rows, size_t stride) {
    size_t idx = 0;
    uint32_t sum = 0;
    for (int r = 0; r < rows; r++) {
        const uint8_t v = base[idx];
        sum += v;
        idx = (size_t)(r + 1) * stride + (v & 63u);
    }
    return sum;
}

static void phase_a1(volatile uint8_t *rows0) {
    constexpr int kIters = 100;
    constexpr int kRows = 21;
    constexpr size_t kStride = 960;
    std::vector<uint32_t> indep(kIters), dep(kIters);
    for (int i = 0; i < kIters; i++) {
        psram_invalidate(rows0, (size_t)kRows * kStride);
        const uint32_t t0 = cycles_now();
        a1_independent(rows0, kRows, kStride);
        indep[i] = cycles_now() - t0;

        psram_invalidate(rows0, (size_t)kRows * kStride);
        const uint32_t t1 = cycles_now();
        a1_dependent(rows0, kRows, kStride);
        dep[i] = cycles_now() - t1;
    }
    psram_report("a1-independent-21x960", indep, kRows);
    psram_report("a1-dependent-21x960", dep, kRows);
}

static void phase_a2(volatile uint8_t *rows0) {
    constexpr int kIters = 100;
    constexpr int kRows = 21;
    constexpr size_t kStride = 960;
    std::vector<uint32_t> issue_wait(kIters), read_wait(kIters);
    std::vector<uint32_t> issue_burst(kIters), read_burst(kIters);
    std::vector<uint32_t> issue_wait128(kIters), read_wait128(kIters);
    int hangs = 0;
    for (int i = 0; i < kIters; i++) {
        psram_invalidate(rows0, (size_t)kRows * kStride);
        uint32_t t0 = cycles_now();
        for (int r = 0; r < kRows; r++) {
            cache_ll_l2_preload(CACHE_LL_ID_ALL, (uint32_t)(uintptr_t)(rows0 + (size_t)r * kStride), 64,
                                CACHE_PRELOAD_ORDER_ASCENDING);
            psram_wait_preload(200000);
        }
        issue_wait[i] = cycles_now() - t0;
        uint32_t t1 = cycles_now();
        psram_read_rows(rows0, kRows, kStride, 32);
        read_wait[i] = cycles_now() - t1;

        psram_invalidate(rows0, (size_t)kRows * kStride);
        uint32_t t2 = cycles_now();
        for (int r = 0; r < kRows; r++) {
            cache_ll_l2_preload(CACHE_LL_ID_ALL, (uint32_t)(uintptr_t)(rows0 + (size_t)r * kStride), 64,
                                CACHE_PRELOAD_ORDER_ASCENDING);
        }
        const bool ok = psram_wait_preload(2000000);
        issue_burst[i] = cycles_now() - t2;
        if (!ok) hangs++;
        uint32_t t3 = cycles_now();
        psram_read_rows(rows0, kRows, kStride, 32);
        read_burst[i] = cycles_now() - t3;

        psram_invalidate(rows0, (size_t)kRows * kStride);
        uint32_t t4 = cycles_now();
        for (int r = 0; r < kRows; r++) {
            cache_ll_l2_preload(CACHE_LL_ID_ALL, (uint32_t)(uintptr_t)(rows0 + (size_t)r * kStride), 128,
                                CACHE_PRELOAD_ORDER_ASCENDING);
            psram_wait_preload(200000);
        }
        issue_wait128[i] = cycles_now() - t4;
        uint32_t t5 = cycles_now();
        psram_read_rows(rows0, kRows, kStride, 32);
        read_wait128[i] = cycles_now() - t5;
    }
    psram_report("a2-issue-64B-wait-each", issue_wait, kRows);
    psram_report("a2-read-after-64B-wait-each", read_wait, kRows);
    psram_report("a2-issue-64B-burst", issue_burst, kRows);
    psram_report("a2-read-after-64B-burst", read_burst, kRows);
    psram_report("a2-issue-128B-wait-each", issue_wait128, kRows);
    psram_report("a2-read-after-128B-wait-each", read_wait128, kRows);
    ESP_LOGI(TAG, "a2-burst-wait-timeouts: %d/%d", hangs, kIters);
}

static void phase_a3(volatile uint8_t *rows0) {
    constexpr int kIters = 100;
    constexpr int kRows = 9;
    constexpr size_t kStride = 1920;
    std::vector<uint32_t> issue_wait(kIters), read_wait(kIters);
    std::vector<uint32_t> issue_burst(kIters), read_burst(kIters);
    int hangs = 0;
    for (int i = 0; i < kIters; i++) {
        psram_invalidate(rows0, (size_t)kRows * kStride);
        uint32_t t0 = cycles_now();
        for (int r = 0; r < kRows; r++) {
            cache_ll_l2_preload(CACHE_LL_ID_ALL, (uint32_t)(uintptr_t)(rows0 + (size_t)r * kStride), 64,
                                CACHE_PRELOAD_ORDER_ASCENDING);
            psram_wait_preload(200000);
        }
        issue_wait[i] = cycles_now() - t0;
        uint32_t t1 = cycles_now();
        psram_read_rows(rows0, kRows, kStride, 16);
        read_wait[i] = cycles_now() - t1;

        psram_invalidate(rows0, (size_t)kRows * kStride);
        uint32_t t2 = cycles_now();
        for (int r = 0; r < kRows; r++) {
            cache_ll_l2_preload(CACHE_LL_ID_ALL, (uint32_t)(uintptr_t)(rows0 + (size_t)r * kStride), 64,
                                CACHE_PRELOAD_ORDER_ASCENDING);
        }
        const bool ok = psram_wait_preload(2000000);
        issue_burst[i] = cycles_now() - t2;
        if (!ok) hangs++;
        uint32_t t3 = cycles_now();
        psram_read_rows(rows0, kRows, kStride, 16);
        read_burst[i] = cycles_now() - t3;
    }
    psram_report("a3-chroma-issue-64B-wait-each", issue_wait, kRows);
    psram_report("a3-chroma-read-after-wait-each", read_wait, kRows);
    psram_report("a3-chroma-issue-64B-burst", issue_burst, kRows);
    psram_report("a3-chroma-read-after-burst", read_burst, kRows);
    ESP_LOGI(TAG, "a3-burst-wait-timeouts: %d/%d", hangs, kIters);
}

static std::atomic<bool> s_a4_go{ false };
static std::atomic<bool> s_a4_done{ false };
static std::atomic<bool> s_a4_stop{ false };
static volatile uint8_t *s_a4_region;

static void a4_worker(void *) {
    while (!s_a4_stop.load(std::memory_order_relaxed)) {
        if (!s_a4_go.load(std::memory_order_acquire)) continue;
        s_a4_go.store(false, std::memory_order_relaxed);
        volatile uint8_t sink = 0;
        for (int r = 0; r < 21; r++) sink ^= s_a4_region[(size_t)r * 960];
        (void)sink;
        s_a4_done.store(true, std::memory_order_release);
    }
    vTaskDeleteWithCaps(nullptr);
}

static void phase_a4(volatile uint8_t *rows0) {
    constexpr int kIters = 100;
    constexpr int kRows = 21;
    constexpr size_t kStride = 960;
    s_a4_stop.store(false);
    s_a4_region = rows0;
    if (h264_create_task(a4_worker, "h264_a4", 2048, nullptr, 5, 1, nullptr) != pdPASS) {
        ESP_LOGE(TAG, "a4: worker spawn failed");
        return;
    }
    std::vector<uint32_t> cross(kIters);
    for (int i = 0; i < kIters; i++) {
        psram_invalidate(rows0, (size_t)kRows * kStride);
        s_a4_done.store(false, std::memory_order_relaxed);
        s_a4_go.store(true, std::memory_order_release);
        while (!s_a4_done.load(std::memory_order_acquire)) {}
        uint32_t t0 = cycles_now();
        psram_read_rows(rows0, kRows, kStride, 32);
        cross[i] = cycles_now() - t0;
    }
    s_a4_stop.store(true);
    psram_report("a4-core0-after-core1-touch", cross, kRows);
}

static void pattern_fill(uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) p[i] = (uint8_t)(seed + i * 131u);
}

static bool pattern_check(const uint8_t *p, size_t n, uint8_t seed) {
    for (size_t i = 0; i < n; i++) {
        if (p[i] != (uint8_t)(seed + i * 131u)) return false;
    }
    return true;
}

static void flush_and_invalidate(void *p, size_t n) {
    esp_cache_msync(p, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE);
}

static std::atomic<bool> s_dma_done{ false };

static bool dma_done_cb(async_memcpy_handle_t, async_memcpy_event_t *, void *) {
    s_dma_done.store(true, std::memory_order_release);
    return false;
}

static bool dma_copy_timed(async_memcpy_handle_t mcp, void *dst, void *src, size_t n,
                           uint32_t *issue_cyc, uint32_t *total_cyc) {
    s_dma_done.store(false, std::memory_order_relaxed);
    const uint32_t t0 = cycles_now();
    const esp_err_t err = esp_async_memcpy(mcp, dst, src, n, dma_done_cb, nullptr);
    const uint32_t t1 = cycles_now();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_async_memcpy(%u bytes) failed: %d", (unsigned)n, (int)err);
        return false;
    }
    uint32_t spins = 0;
    while (!s_dma_done.load(std::memory_order_acquire)) {
        if (++spins > 20000000u) {
            ESP_LOGE(TAG, "dma: timed out waiting for completion");
            return false;
        }
    }
    const uint32_t t2 = cycles_now();
    *issue_cyc = t1 - t0;
    *total_cyc = t2 - t0;
    return true;
}

static void dma_measure(async_memcpy_handle_t mcp, const char *dir, uint8_t *dst, uint8_t *src,
                        size_t n) {
    constexpr int kIters = 100;
    std::vector<uint32_t> issue(kIters), total(kIters);
    int mismatches = 0;
    for (int i = 0; i < kIters; i++) {
        const uint8_t seed = (uint8_t)(i * 37 + 1);
        pattern_fill(src, n, seed);
        memset(dst, 0, n);
        if (!dma_copy_timed(mcp, dst, src, n, &issue[i], &total[i])) return;
        psram_invalidate(dst, n);
        if (!pattern_check(dst, n, seed)) mismatches++;
    }
    char label[48];
    snprintf(label, sizeof(label), "dma-%s-%uB-issue", dir, (unsigned)n);
    psram_report(label, issue, n);
    snprintf(label, sizeof(label), "dma-%s-%uB-total", dir, (unsigned)n);
    psram_report(label, total, n);
    ESP_LOGI(TAG, "dma-%s-%uB mismatches: %d/%d", dir, (unsigned)n, mismatches, kIters);
}

static void cpu_measure(const char *dir, uint8_t *dst, uint8_t *src, size_t n, bool cold_src) {
    constexpr int kIters = 100;
    std::vector<uint32_t> cyc(kIters);
    int mismatches = 0;
    for (int i = 0; i < kIters; i++) {
        const uint8_t seed = (uint8_t)(i * 37 + 1);
        pattern_fill(src, n, seed);
        memset(dst, 0, n);
        if (cold_src) flush_and_invalidate(src, n);
        const uint32_t t0 = cycles_now();
        memcpy(dst, src, n);
        esp_cache_msync(dst, n, ESP_CACHE_MSYNC_FLAG_DIR_C2M);
        cyc[i] = cycles_now() - t0;
        psram_invalidate(dst, n);
        if (!pattern_check(dst, n, seed)) mismatches++;
    }
    char label[48];
    snprintf(label, sizeof(label), "cpu-%s-%uB", dir, (unsigned)n);
    psram_report(label, cyc, n);
    ESP_LOGI(TAG, "cpu-%s-%uB mismatches: %d/%d", dir, (unsigned)n, mismatches, kIters);
}

static void phase_b(uint8_t *psram_buf, uint8_t *sram_buf) {
    static const size_t kSizes[] = { 8640, 15360, 47104 };
    async_memcpy_config_t cfg = ASYNC_MEMCPY_DEFAULT_CONFIG();
    async_memcpy_handle_t mcp = nullptr;
    const esp_err_t err = esp_async_memcpy_install_gdma_axi(&cfg, &mcp);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "phase_b: install failed: %d", (int)err);
        return;
    }
    for (size_t n : kSizes) {
        dma_measure(mcp, "p2s", sram_buf, psram_buf, n);
        dma_measure(mcp, "s2p", psram_buf, sram_buf, n);
        cpu_measure("p2s", sram_buf, psram_buf, n, true);
        cpu_measure("s2p", psram_buf, sram_buf, n, false);
    }
    esp_async_memcpy_uninstall(mcp);
}

static void psram_bench_run() {
    constexpr size_t kFrameBytes = 400 * 1024;
    constexpr size_t kDmaMaxBytes = 47104;
    auto *frame = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, kFrameBytes, MALLOC_CAP_SPIRAM));
    auto *dma_psram = static_cast<uint8_t *>(heap_caps_aligned_alloc(64, kDmaMaxBytes, MALLOC_CAP_SPIRAM));
    if (frame) {
        memset(frame, 0x5a, kFrameBytes);
        s_ticks_per_us = esp_rom_get_cpu_ticks_per_us();
        volatile uint8_t *rows0 = frame + 4096;
        phase_a1(rows0);
        phase_a2(rows0);
        phase_a3(rows0);
        phase_a4(rows0);
    } else {
        ESP_LOGE(TAG, "psram bench: no memory");
    }
    if (dma_psram) {
        lv_lock();
        const SharedSram sram = media_player_acquire_sram();
        lv_unlock();
        if (sram.base && sram.bytes >= kDmaMaxBytes) {
            phase_b(dma_psram, static_cast<uint8_t *>(sram.base));
        } else {
            ESP_LOGE(TAG, "psram bench: shared sram unavailable for dma phase");
        }
        lv_lock();
        media_player_release_sram();
        lv_unlock();
    } else {
        ESP_LOGE(TAG, "psram bench: no memory for dma psram buffer");
    }
    heap_caps_free(frame);
    heap_caps_free(dma_psram);
}

static void psram_bench_task(void *) {
    psram_bench_run();
    ESP_LOGI(TAG, "psram bench done");
    s_busy.store(false);
    vTaskDeleteWithCaps(nullptr);
}

static bool psram_bench_command(int, const char *const *, void *) {
    if (s_busy.exchange(true)) return false;
    if (h264_create_task(psram_bench_task, "h264_psram", kBenchStackBytes, nullptr, 5, 0, nullptr) !=
        pdPASS) {
        s_busy.store(false);
        return false;
    }
    return true;
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
    harness_register("h264psrambench", psram_bench_command, nullptr);
}

#else

void h264_bench_register() {}

#endif
