/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "opus_bench.hpp"

#if defined(ESP_PLATFORM) && defined(OPUS_BENCH)

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
#include "audf_opus.h"
#include "harness.h"
#include "video/video_threads.hpp"

static const char *TAG = "OPUS";

extern const uint8_t bench_music_start[] asm("_binary_music_opus_start");
extern const uint8_t bench_music_end[] asm("_binary_music_opus_end");
extern const uint8_t bench_hybrid_start[] asm("_binary_hybrid_opus_start");
extern const uint8_t bench_hybrid_end[] asm("_binary_hybrid_opus_end");
extern const uint8_t bench_speech_start[] asm("_binary_speech_opus_start");
extern const uint8_t bench_speech_end[] asm("_binary_speech_opus_end");

static constexpr uint32_t kBenchStackBytes = 16384;
static constexpr uint32_t kProbeStackBytes = 8192;

struct BenchArgs {
    int loops;
    const uint8_t *clip;
    std::size_t clip_bytes;
    uint32_t caps;
};

struct Packet {
    uint32_t offset;
    uint32_t len;
};

static std::atomic<bool> s_busy{false};

struct StackProbe {
    audf_decoder_t *dec;
    const uint8_t *buf;
    const std::vector<Packet> *packets;
    int16_t *pcm;
    TaskHandle_t parent;
    UBaseType_t free_bytes;
};

/* Decodes the clip once on a stack of its own, to see how deep decoding goes. */
static void stack_probe_task(void *arg) {
    auto *probe = static_cast<StackProbe *>(arg);
    for (std::size_t i = 2; i < probe->packets->size(); i++) {
        const Packet &p = (*probe->packets)[i];
        std::size_t n = 0;
        audf_decoder_decode(probe->dec, probe->buf + p.offset, p.len, probe->pcm, &n);
    }
    probe->free_bytes = uxTaskGetStackHighWaterMark(nullptr);
    xTaskNotifyGive(probe->parent);
    vTaskDeleteWithCaps(nullptr);
}

/* Packets of the first stream, joined across pages into `out`: the OpusHead
   first, then OpusTags, then audio. */
static std::vector<Packet> split_packets(const uint8_t *data, std::size_t size, uint8_t *out) {
    std::vector<Packet> packets;
    std::size_t pos = 0, used = 0, start = 0;
    while (pos + 27 <= size && memcmp(data + pos, "OggS", 4) == 0) {
        const int segments = data[pos + 26];
        const uint8_t *lacing = data + pos + 27;
        std::size_t body = pos + 27 + segments;
        for (int i = 0; i < segments; i++) {
            memcpy(out + used, data + body, lacing[i]);
            used += lacing[i];
            body += lacing[i];
            if (lacing[i] < 255) {
                packets.push_back({ (uint32_t)start, (uint32_t)(used - start) });
                start = used;
            }
        }
        pos = body;
    }
    return packets;
}

static void run_audf(BenchArgs *args, const uint8_t *buf, const std::vector<Packet> &packets) {
    audf_opus_config_t config = {};
    config.head = buf + packets[0].offset;
    config.head_len = packets[0].len;
    config.alloc_caps = args->caps;
    audf_decoder_t *dec = nullptr;
    if (audf_opus_decoder_create(&config, &dec) != ESP_OK) {
        ESP_LOGE(TAG, "cannot create the decoder");
        return;
    }
    auto *pcm = static_cast<int16_t *>(
        heap_caps_malloc(audf_decoder_max_frames(dec) * audf_decoder_channels(dec) * 2, MALLOC_CAP_SPIRAM));
    const double cpu_hz = (double)CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ * 1e6;
    StackProbe probe = { dec, buf, &packets, pcm, xTaskGetCurrentTaskHandle(), 0 };
    TaskHandle_t task = nullptr;
    if (pcm && xTaskCreatePinnedToCoreWithCaps(stack_probe_task, "opus_stack", kProbeStackBytes, &probe, 5,
                                               &task, 1, MALLOC_CAP_SPIRAM) == pdPASS) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        ESP_LOGI(TAG, "stack: %u of %u bytes used by a task that only decodes",
                 (unsigned)(kProbeStackBytes - probe.free_bytes), (unsigned)kProbeStackBytes);
    }
    for (int loop = 0; pcm && loop < args->loops; loop++) {
        audf_decoder_reset(dec);
        uint64_t cycles = 0, worst = 0, samples = 0;
        int errors = 0;
        for (std::size_t i = 2; i < packets.size(); i++) {
            std::size_t n = 0;
            const uint32_t t0 = esp_cpu_get_cycle_count();
            const esp_err_t e = audf_decoder_decode(dec, buf + packets[i].offset, packets[i].len, pcm, &n);
            const uint32_t dt = esp_cpu_get_cycle_count() - t0;
            cycles += dt;
            if (dt > worst) worst = dt;
            if (e != ESP_OK) {
                errors++;
                continue;
            }
            samples += n;
        }
        const double secs = (double)samples / AUDF_OPUS_RATE;
        ESP_LOGI(TAG, "loop %d: %u packets, %d errors, x%u, %.0f cycles/packet (worst %" PRIu64
                 "), %.2f%% of a core",
                 loop, (unsigned)(packets.size() - 2), errors, (unsigned)audf_decoder_channels(dec),
                 packets.size() > 2 ? (double)cycles / (packets.size() - 2) : 0.0, worst,
                 secs > 0 ? 100.0 * cycles / cpu_hz / secs : 0.0);
    }
    heap_caps_free(pcm);
    audf_decoder_destroy(dec);
}

static void bench_task(void *arg) {
    auto *args = static_cast<BenchArgs *>(arg);
    auto *buf = static_cast<uint8_t *>(heap_caps_malloc(args->clip_bytes, MALLOC_CAP_SPIRAM));
    if (buf) {
        const std::vector<Packet> packets = split_packets(args->clip, args->clip_bytes, buf);
        if (packets.size() > 2 && packets[0].len >= AUDF_OPUS_HEAD_BYTES) {
            run_audf(args, buf, packets);
        } else {
            ESP_LOGE(TAG, "cannot set up the clip");
        }
    }
    heap_caps_free(buf);
    ESP_LOGI(TAG, "bench done");
    delete args;
    s_busy.store(false);
    vTaskDeleteWithCaps(nullptr);
}

static bool bench_command(int argc, const char *const *argv, void *) {
    const int loops = argc > 1 ? atoi(argv[1]) : 3;
    if (loops <= 0 || s_busy.exchange(true)) return false;
    auto *args = new BenchArgs{ loops, bench_music_start,
                                (std::size_t)(bench_music_end - bench_music_start), MALLOC_CAP_SPIRAM };
    for (int i = 2; i < argc; i++) {
        if (strcmp(argv[i], "hybrid") == 0) {
            args->clip = bench_hybrid_start;
            args->clip_bytes = (std::size_t)(bench_hybrid_end - bench_hybrid_start);
        } else if (strcmp(argv[i], "speech") == 0) {
            args->clip = bench_speech_start;
            args->clip_bytes = (std::size_t)(bench_speech_end - bench_speech_start);
        } else if (strcmp(argv[i], "internal") == 0) {
            args->caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
        }
    }
    if (video_create_task(bench_task, "opus_bench", kBenchStackBytes, args, 5, 1, nullptr) != pdPASS) {
        delete args;
        s_busy.store(false);
        return false;
    }
    return true;
}

void opus_bench_register() {
    harness_register("opusbench", bench_command, nullptr);
}

#else

void opus_bench_register() {}

#endif
