/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "mp3_bench.hpp"

#if defined(ESP_PLATFORM) && defined(MP3_BENCH)

#include <atomic>
#include <cinttypes>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "esp_cpu.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "audf_mp3.h"
#include "harness.h"
#include "video/video_threads.hpp"

static const char *TAG = "MP3";

extern const uint8_t bench_cbr128_start[] asm("_binary_cbr128_mp3_start");
extern const uint8_t bench_cbr128_end[] asm("_binary_cbr128_mp3_end");
extern const uint8_t bench_cbr320_start[] asm("_binary_cbr320_mp3_start");
extern const uint8_t bench_cbr320_end[] asm("_binary_cbr320_mp3_end");
extern const uint8_t bench_layer2_start[] asm("_binary_layer2_mp2_start");
extern const uint8_t bench_layer2_end[] asm("_binary_layer2_mp2_end");

static constexpr uint32_t kBenchStackBytes = 8192;
static constexpr uint32_t kProbeStackBytes = 8192;

struct BenchArgs {
    int loops;
    const uint8_t *clip;
    std::size_t clip_bytes;
    uint32_t caps;
};

struct Frame {
    uint32_t offset;
    uint32_t len;
};

struct StackProbe {
    audf_decoder_t *dec;
    const uint8_t *clip;
    const std::vector<Frame> *frames;
    int16_t *pcm;
    TaskHandle_t parent;
    UBaseType_t free_bytes;
};

static std::atomic<bool> s_busy{false};

static std::vector<Frame> split_frames(const uint8_t *data, std::size_t size, audf_mp3_header_t *first) {
    std::vector<Frame> frames;
    for (std::size_t pos = 0; pos + AUDF_MP3_HEADER_BYTES <= size;) {
        audf_mp3_header_t h;
        if (!audf_mp3_parse_header(data + pos, &h)) {
            pos++;
            continue;
        }
        if (pos + h.bytes > size) break;
        if (frames.empty()) *first = h;
        frames.push_back({ (uint32_t)pos, h.bytes });
        pos += h.bytes;
    }
    return frames;
}

static void stack_probe_task(void *arg) {
    auto *probe = static_cast<StackProbe *>(arg);
    for (const Frame &f : *probe->frames) {
        std::size_t n = 0;
        audf_decoder_decode(probe->dec, probe->clip + f.offset, f.len, probe->pcm, &n);
    }
    probe->free_bytes = uxTaskGetStackHighWaterMark(nullptr);
    xTaskNotifyGive(probe->parent);
    vTaskDeleteWithCaps(nullptr);
}

static void bench_task(void *arg) {
    auto *args = static_cast<BenchArgs *>(arg);
    auto *clip = static_cast<uint8_t *>(heap_caps_malloc(args->clip_bytes, MALLOC_CAP_SPIRAM));
    auto *pcm = static_cast<int16_t *>(
        heap_caps_malloc(AUDF_MP3_MAX_FRAMES * 2 * sizeof(int16_t), MALLOC_CAP_SPIRAM));
    audf_decoder_t *dec = nullptr;
    audf_mp3_header_t first = {};
    std::vector<Frame> frames;
    if (clip && pcm) {
        memcpy(clip, args->clip, args->clip_bytes);
        frames = split_frames(clip, args->clip_bytes, &first);
        audf_mp3_config_t config = {};
        config.channels = first.channels;
        config.alloc_caps = args->caps;
        if (!frames.empty() && audf_mp3_decoder_create(&config, &dec) != ESP_OK) dec = nullptr;
    }
    if (dec) {
        StackProbe probe = { dec, clip, &frames, pcm, xTaskGetCurrentTaskHandle(), 0 };
        TaskHandle_t task = nullptr;
        if (xTaskCreatePinnedToCoreWithCaps(stack_probe_task, "mp3_stack", kProbeStackBytes, &probe, 5, &task,
                                            1, MALLOC_CAP_SPIRAM) == pdPASS) {
            ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
            ESP_LOGI(TAG, "stack: %u of %u bytes used by a task that only decodes",
                     (unsigned)(kProbeStackBytes - probe.free_bytes), (unsigned)kProbeStackBytes);
        }
        const double cpu_hz = (double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1e6;
        for (int loop = 0; loop < args->loops; loop++) {
            audf_decoder_reset(dec);
            uint64_t cycles = 0, worst = 0, samples = 0;
            int errors = 0;
            for (const Frame &f : frames) {
                std::size_t n = 0;
                const uint32_t t0 = esp_cpu_get_cycle_count();
                const esp_err_t e = audf_decoder_decode(dec, clip + f.offset, f.len, pcm, &n);
                const uint32_t dt = esp_cpu_get_cycle_count() - t0;
                cycles += dt;
                if (dt > worst) worst = dt;
                if (e != ESP_OK) {
                    errors++;
                    continue;
                }
                samples += n;
            }
            const double secs = (double)samples / first.rate;
            ESP_LOGI(TAG, "loop %d: Layer %s, %u frames, %d errors, %u Hz x%u, %.0f cycles/frame"
                     " (worst %" PRIu64 "), %.2f%% of a core",
                     loop, first.layer == 2 ? "II" : "III", (unsigned)frames.size(), errors,
                     (unsigned)first.rate, (unsigned)first.channels, (double)cycles / frames.size(), worst,
                     secs > 0 ? 100.0 * cycles / cpu_hz / secs : 0.0);
        }
    } else {
        ESP_LOGE(TAG, "cannot set up the clip");
    }
    audf_decoder_destroy(dec);
    heap_caps_free(pcm);
    heap_caps_free(clip);
    ESP_LOGI(TAG, "bench done");
    delete args;
    s_busy.store(false);
    vTaskDeleteWithCaps(nullptr);
}

static bool bench_command(int argc, const char *const *argv, void *) {
    const int loops = argc > 1 ? atoi(argv[1]) : 3;
    if (loops <= 0 || s_busy.exchange(true)) return false;
    auto *args = new BenchArgs{ loops, bench_cbr128_start,
                                (std::size_t)(bench_cbr128_end - bench_cbr128_start), MALLOC_CAP_SPIRAM };
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "320") == 0) {
            args->clip = bench_cbr320_start;
            args->clip_bytes = (std::size_t)(bench_cbr320_end - bench_cbr320_start);
        } else if (strcmp(argv[i], "mp2") == 0) {
            args->clip = bench_layer2_start;
            args->clip_bytes = (std::size_t)(bench_layer2_end - bench_layer2_start);
        } else if (strcmp(argv[i], "internal") == 0) {
            args->caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        }
    }
    if (video_create_task(bench_task, "mp3_bench", kBenchStackBytes, args, 5, 1, nullptr) != pdPASS) {
        delete args;
        s_busy.store(false);
        return false;
    }
    return true;
}

void mp3_bench_register() {
    harness_register("mp3bench", bench_command, nullptr);
}

#else

void mp3_bench_register() {}

#endif
