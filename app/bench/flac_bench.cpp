/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "flac_bench.hpp"

#if defined(ESP_PLATFORM) && defined(FLAC_BENCH)

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
#include "audf_flac.h"
#include "harness.h"
#include "video/video_threads.hpp"

static const char *TAG = "FLAC";

extern const uint8_t bench_cd_start[] asm("_binary_cd_flac_start");
extern const uint8_t bench_cd_end[] asm("_binary_cd_flac_end");
extern const uint8_t bench_hires_start[] asm("_binary_hires_flac_start");
extern const uint8_t bench_hires_end[] asm("_binary_hires_flac_end");

static constexpr uint32_t kBenchStackBytes = 8192;

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

static std::atomic<bool> s_busy{false};

static const uint8_t *find_streaminfo(const uint8_t *data, std::size_t size, std::size_t *first) {
    if (size < 4 || memcmp(data, "fLaC", 4) != 0) return nullptr;
    const uint8_t *streaminfo = nullptr;
    std::size_t pos = 4;
    for (bool last = false; !last && pos + 4 <= size;) {
        last = data[pos] & 0x80;
        const std::size_t len = (std::size_t)data[pos + 1] << 16 | data[pos + 2] << 8 | data[pos + 3];
        if ((data[pos] & 0x7f) == 0) streaminfo = data + pos + 4;
        pos += 4 + len;
    }
    *first = pos;
    return pos <= size ? streaminfo : nullptr;
}

static std::vector<Frame> split_frames(audf_decoder_t *dec, const uint8_t *data, std::size_t size,
                                       std::size_t pos, void *pcm) {
    std::vector<Frame> frames;
    while (pos + 2 <= size) {
        std::size_t end = pos + 2;
        std::size_t n = 0;
        for (; end + 2 <= size; end++) {
            if (data[end] != 0xff || (data[end + 1] & 0xfe) != 0xf8) continue;
            if (audf_decoder_decode(dec, data + pos, end - pos, pcm, &n) == ESP_OK) break;
        }
        if (end + 2 > size) end = size;
        frames.push_back({ (uint32_t)pos, (uint32_t)(end - pos) });
        pos = end;
    }
    return frames;
}

static void bench_task(void *arg) {
    auto *args = static_cast<BenchArgs *>(arg);
    auto *clip = static_cast<uint8_t *>(heap_caps_malloc(args->clip_bytes, MALLOC_CAP_SPIRAM));
    std::size_t first = 0;
    const uint8_t *streaminfo = clip ? find_streaminfo(args->clip, args->clip_bytes, &first) : nullptr;
    audf_decoder_t *dec = nullptr;
    void *pcm = nullptr;
    if (streaminfo) {
        memcpy(clip, args->clip, args->clip_bytes);
        audf_flac_config_t config = {};
        config.streaminfo = clip + (streaminfo - args->clip);
        config.streaminfo_len = AUDF_FLAC_STREAMINFO_BYTES;
        config.alloc_caps = args->caps;
        if (audf_flac_decoder_create(&config, &dec) == ESP_OK) {
            pcm = heap_caps_malloc(audf_decoder_max_frames(dec) * audf_decoder_channels(dec) * 4,
                                   MALLOC_CAP_SPIRAM);
        }
    }
    if (dec && pcm) {
        const std::vector<Frame> frames = split_frames(dec, clip, args->clip_bytes, first, pcm);
        const uint32_t rate = audf_flac_decoder_rate(dec);
        const double cpu_hz = (double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1e6;
        for (int loop = 0; loop < args->loops; loop++) {
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
            const double secs = (double)samples / rate;
            ESP_LOGI(TAG, "loop %d: %u frames, %d errors, %u Hz x%u, %.0f cycles/frame (worst %" PRIu64
                     "), %.2f%% of a core",
                     loop, (unsigned)frames.size(), errors, (unsigned)rate,
                     (unsigned)audf_decoder_channels(dec),
                     frames.empty() ? 0.0 : (double)cycles / frames.size(), worst,
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
    auto *args = new BenchArgs{ loops, bench_cd_start, (std::size_t)(bench_cd_end - bench_cd_start),
                                MALLOC_CAP_SPIRAM };
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "hires") == 0) {
            args->clip = bench_hires_start;
            args->clip_bytes = (std::size_t)(bench_hires_end - bench_hires_start);
        } else if (strcmp(argv[i], "internal") == 0) {
            args->caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        }
    }
    if (video_create_task(bench_task, "flac_bench", kBenchStackBytes, args, 5, 1, nullptr) != pdPASS) {
        delete args;
        s_busy.store(false);
        return false;
    }
    return true;
}

void flac_bench_register() {
    harness_register("flacbench", bench_command, nullptr);
}

#else

void flac_bench_register() {}

#endif
