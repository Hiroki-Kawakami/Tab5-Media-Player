/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "aac_bench.hpp"

#if defined(ESP_PLATFORM) && defined(AAC_BENCH)

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
#include "audf_aac.h"
#include "harness.h"
#include "video/video_threads.hpp"

extern "C" {
#include "esp_aac_dec.h"
#include "esp_audio_dec.h"
}

static const char *TAG = "AAC";

extern const uint8_t bench_lc_start[] asm("_binary_lc_aac_start");
extern const uint8_t bench_lc_end[] asm("_binary_lc_aac_end");
extern const uint8_t bench_he_start[] asm("_binary_he_aac_start");
extern const uint8_t bench_he_end[] asm("_binary_he_aac_end");
extern const uint8_t bench_ps_start[] asm("_binary_ps_aac_start");
extern const uint8_t bench_ps_end[] asm("_binary_ps_aac_end");

static constexpr uint32_t kBenchStackBytes = 8192;
static constexpr std::size_t kPcmBytes = 2048 * 2 * sizeof(int16_t);

enum class He { Off, V1, V2 };

struct BenchArgs {
    int loops;
    const uint8_t *clip;
    std::size_t clip_bytes;
    He he;
    bool hash;
    bool esp;
    uint32_t caps;
};

struct Frame {
    uint32_t offset;
    uint32_t len;
};

static std::atomic<bool> s_busy{false};

static std::vector<Frame> split_adts(const uint8_t *data, std::size_t size) {
    std::vector<Frame> frames;
    std::size_t pos = 0;
    while (pos + 7 <= size) {
        if (data[pos] != 0xFF || (data[pos + 1] & 0xF6) != 0xF0) break;
        const uint32_t len = ((data[pos + 3] & 3u) << 11) | (data[pos + 4] << 3) | (data[pos + 5] >> 5);
        if (len < 7 || pos + len > size) break;
        frames.push_back({ (uint32_t)pos, len });
        pos += len;
    }
    return frames;
}

static uint32_t fnv1a(uint32_t h, const uint8_t *data, std::size_t len) {
    for (std::size_t i = 0; i < len; i++) h = (h ^ data[i]) * 16777619u;
    return h;
}

static void report(int loop, int frames, uint64_t cycles, uint64_t worst, uint64_t samples,
                   uint32_t rate, uint32_t hash, bool with_hash) {
    const double cpu_hz = (double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1e6;
    const double secs = rate ? (double)samples / rate : 0;
    const double load = secs > 0 ? 100.0 * cycles / cpu_hz / secs : 0;
    ESP_LOGI(TAG, "loop %d: %d frames, %u Hz, %.0f cycles/frame (worst %" PRIu64 "), %.2f%% of a core",
             loop, frames, (unsigned)rate, frames ? (double)cycles / frames : 0, worst, load);
    if (with_hash) printf("[AACPCM] h=%08" PRIx32 " samples=%" PRIu64 "\n", hash, samples);
}

static void run_esp(const BenchArgs *args, const std::vector<Frame> &frames, uint8_t *pcm) {
    esp_aac_dec_register();
    esp_aac_dec_cfg_t aac = {};
    aac.aac_plus_enable = args->he != He::Off;
    esp_audio_dec_cfg_t config = {};
    config.type = ESP_AUDIO_TYPE_AAC;
    config.cfg = &aac;
    config.cfg_sz = sizeof(aac);
    esp_audio_dec_handle_t dec = nullptr;
    if (esp_audio_dec_open(&config, &dec) != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "esp_audio_dec_open failed");
        return;
    }
    for (int loop = 0; loop < args->loops; loop++) {
        esp_audio_dec_reset(dec);
        uint64_t cycles = 0;
        uint64_t worst = 0;
        uint64_t samples = 0;
        uint32_t rate = 0;
        uint32_t hash = 2166136261u;
        for (const Frame &f : frames) {
            esp_audio_dec_in_raw_t raw = {};
            raw.buffer = const_cast<uint8_t *>(args->clip + f.offset);
            raw.len = f.len;
            esp_audio_dec_out_frame_t out = {};
            out.buffer = pcm;
            out.len = kPcmBytes;
            const uint32_t t0 = esp_cpu_get_cycle_count();
            const esp_audio_err_t err = esp_audio_dec_process(dec, &raw, &out);
            const uint32_t dt = esp_cpu_get_cycle_count() - t0;
            cycles += dt;
            if (dt > worst) worst = dt;
            if (err != ESP_AUDIO_ERR_OK) continue;
            esp_audio_dec_info_t info = {};
            esp_audio_dec_get_info(dec, &info);
            rate = info.sample_rate;
            if (info.channel) samples += out.decoded_size / (2u * info.channel);
            if (args->hash && loop == 0) hash = fnv1a(hash, pcm, out.decoded_size);
        }
        report(loop, (int)frames.size(), cycles, worst, samples, rate, hash, args->hash && loop == 0);
    }
    esp_audio_dec_close(dec);
}

static uint32_t cycles_now() {
    return (uint32_t)esp_cpu_get_cycle_count();
}

static void run_audf(const BenchArgs *args, const std::vector<Frame> &frames, uint8_t *pcm) {
    audf_aac_config_t config = {};
    config.asc = args->clip;
    config.asc_len = frames.empty() ? 0 : frames[0].len;
    config.adts = true;
    config.he = args->he == He::Off ? AUDF_AAC_HE_OFF : args->he == He::V1 ? AUDF_AAC_HE_V1 : AUDF_AAC_HE_V2;
    config.alloc_caps = args->caps;
    config.clock = cycles_now;
    audf_decoder_t *dec = nullptr;
    const esp_err_t err = audf_aac_decoder_create(&config, &dec);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "audf_aac_decoder_create: %s", esp_err_to_name(err));
        return;
    }
    const uint32_t rate = audf_aac_decoder_rate(dec);
    const uint8_t channels = audf_decoder_channels(dec);
    for (int loop = 0; loop < args->loops; loop++) {
        audf_decoder_reset(dec);
        uint64_t cycles = 0;
        uint64_t worst = 0;
        uint64_t samples = 0;
        uint32_t hash = 2166136261u;
        int errors = 0;
        for (const Frame &f : frames) {
            std::size_t n = 0;
            const uint32_t t0 = esp_cpu_get_cycle_count();
            const esp_err_t e = audf_decoder_decode(dec, args->clip + f.offset, f.len, pcm, &n);
            const uint32_t dt = esp_cpu_get_cycle_count() - t0;
            cycles += dt;
            if (dt > worst) worst = dt;
            if (e != ESP_OK) {
                errors++;
                continue;
            }
            samples += n;
            if (args->hash) hash = fnv1a(hash, pcm, n * channels * sizeof(int16_t));
        }
        if (errors) ESP_LOGW(TAG, "%d frames failed", errors);
        report(loop, (int)frames.size(), cycles, worst, samples, rate, hash, args->hash);
        uint64_t profile[AUDF_AAC_PROF_COUNT];
        if (audf_aac_take_profile(dec, profile) && !frames.empty()) {
            static const char *const kNames[] = { "core", "filterbank", "qmf_ana", "sbr", "ps", "qmf_syn" };
            char line[256];
            int n = 0;
            for (int i = 0; i < AUDF_AAC_PROF_COUNT; i++) {
                n += snprintf(line + n, sizeof(line) - n, "%s %.0f ", kNames[i], (double)profile[i] / frames.size());
            }
            ESP_LOGI(TAG, "cycles/frame by stage: %s", line);
        }
    }
    audf_decoder_destroy(dec);
}

static void bench_task(void *arg) {
    auto *args = static_cast<BenchArgs *>(arg);
    auto *pcm = static_cast<uint8_t *>(heap_caps_aligned_alloc(16, kPcmBytes, MALLOC_CAP_SIMD));
    auto *clip = static_cast<uint8_t *>(heap_caps_malloc(args->clip_bytes, MALLOC_CAP_SPIRAM));
    if (pcm && clip) {
        memcpy(clip, args->clip, args->clip_bytes);
        args->clip = clip;
        const std::vector<Frame> frames = split_adts(clip, args->clip_bytes);
        ESP_LOGI(TAG, "%u frames, internal free %u", (unsigned)frames.size(),
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
        if (args->esp) {
            run_esp(args, frames, pcm);
        } else {
            run_audf(args, frames, pcm);
        }
    } else {
        ESP_LOGE(TAG, "no memory");
    }
    heap_caps_free(clip);
    heap_caps_free(pcm);
    ESP_LOGI(TAG, "bench done");
    delete args;
    s_busy.store(false);
    vTaskDeleteWithCaps(nullptr);
}

static bool bench_command(int argc, const char *const *argv, void *) {
    const int loops = argc > 1 ? atoi(argv[1]) : 3;
    if (loops <= 0 || s_busy.exchange(true)) return false;
    auto *args = new BenchArgs{ loops, bench_lc_start, (std::size_t)(bench_lc_end - bench_lc_start),
                                He::V2, false, false, MALLOC_CAP_SPIRAM };
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "lc") == 0) {
            args->clip = bench_lc_start;
            args->clip_bytes = (std::size_t)(bench_lc_end - bench_lc_start);
        } else if (strcmp(argv[i], "he") == 0) {
            args->clip = bench_he_start;
            args->clip_bytes = (std::size_t)(bench_he_end - bench_he_start);
        } else if (strcmp(argv[i], "ps") == 0) {
            args->clip = bench_ps_start;
            args->clip_bytes = (std::size_t)(bench_ps_end - bench_ps_start);
        } else if (strcmp(argv[i], "off") == 0) {
            args->he = He::Off;
        } else if (strcmp(argv[i], "v1") == 0) {
            args->he = He::V1;
        } else if (strcmp(argv[i], "v2") == 0) {
            args->he = He::V2;
        } else if (strcmp(argv[i], "hash") == 0) {
            args->hash = true;
        } else if (strcmp(argv[i], "esp") == 0) {
            args->esp = true;
        } else if (strcmp(argv[i], "internal") == 0) {
            args->caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        }
    }
    if (video_create_task(bench_task, "aac_bench", kBenchStackBytes, args, 5, 1, nullptr) != pdPASS) {
        delete args;
        s_busy.store(false);
        return false;
    }
    return true;
}

static bool kernel_test_command(int argc, const char *const *argv, void *) {
    const int iterations = argc > 1 ? atoi(argv[1]) : 1000;
    const uint32_t t0 = esp_cpu_get_cycle_count();
    const int bad = audf_aac_kernel_selftest(t0 | 1, iterations);
    ESP_LOGI(TAG, "kernel selftest: %d iterations, %d mismatches", iterations, bad);
    return bad == 0;
}

void aac_bench_register() {
    harness_register("aackerneltest", kernel_test_command, nullptr);
    harness_register("aacbench", bench_command, nullptr);
}

#else

void aac_bench_register() {}

#endif
