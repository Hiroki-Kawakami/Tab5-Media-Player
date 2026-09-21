/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "sdkconfig.h"

#include "media_cache.hpp"
#include "media/media_probe.hpp"
#include "media/psram_allocator.hpp"
#include "playback/player.hpp"
#include "media_player.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "harness.h"
#include "lvgl.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <cstring>
#include <memory>
#include <new>
#include <utility>

static const char *TAG = "media_cache";

static constexpr std::size_t kMetaEntries = 512;
static constexpr std::size_t kRawBudget = 1024 * 1024;
static constexpr std::size_t kJpegBudget = 2 * 1024 * 1024;
static constexpr std::size_t kDecodedBudget = 2 * 1024 * 1024;
static constexpr int32_t kRawMaxSide = 128;
static constexpr uint32_t kWorkerStackBytes = 8192;
static constexpr uint32_t kDispatchPeriodMs = 100;
static constexpr uint32_t kPlayingGapMs = 200;
static constexpr uint32_t kStopTimeoutMs = 2000;
static constexpr std::size_t kHashSkipBytes = 1024;
static constexpr std::size_t kHashWindowBytes = 8 * 1024;
/* One cover queued for the decoder while the reader fetches the next: enough
   to hide the decode behind the SD read without holding a third picture. */
static constexpr uint32_t kDecodeDepth = 1;

namespace {

struct ImageKey {
    uint64_t id;
    int32_t side;

    bool operator==(const ImageKey &other) const {
        return id == other.id && side == other.side;
    }
};

struct ImageKeyHash {
    std::size_t operator()(const ImageKey &key) const {
        return (std::size_t)(key.id ^ ((uint64_t)key.side * 0x9E3779B97F4A7C15ull));
    }
};

using JpegBytes = PsramVector<uint8_t>;

struct ImageEntry {
    std::shared_ptr<CoverPixels> pixels;
    std::shared_ptr<JpegBytes> jpeg;
    std::size_t bytes = 0;
    uint64_t used = 0;
    bool rgb888 = false;
};

struct ImageStore {
    PsramMap<ImageKey, ImageEntry, ImageKeyHash> map;
    std::size_t bytes = 0;
    std::size_t budget = 0;
};

struct Request {
    PsramString path;
    uint8_t want = 0;
    int32_t side = 0;
    uint32_t token = 0;
};

struct MetaSlot {
    std::shared_ptr<MediaEntry> entry;
    uint64_t used = 0;
};

struct DecodeJob {
    PsramString path;
    std::shared_ptr<MediaEntry> entry;
    CoverArt cover;
    std::shared_ptr<JpegBytes> jpeg;
    ImageKey key;
    uint32_t token = 0;
};

struct Observer {
    uint32_t token;
    void (*on_ready)(const std::string &);
};

struct CacheState {
    PsramMap<PsramString, MetaSlot, PsramStringHash> meta;
    PsramMap<uint64_t, uint32_t> uses;
    ImageStore raw;
    ImageStore jpeg;
    ImageStore decoded;
    PsramDeque<Request> queues[3];
    PsramDeque<DecodeJob> decodes;
    PsramVector<PsramString> completed;
    PsramVector<Observer> observers;
    uint64_t clock = 0;
    uint32_t next_token = 1;
};

}

static media_arena_t s_arena;
static SemaphoreHandle_t s_lock;
static SemaphoreHandle_t s_wake;
static SemaphoreHandle_t s_decode_wake;
static SemaphoreHandle_t s_decode_room;
static SemaphoreHandle_t s_stopped;
static SemaphoreHandle_t s_decoder_stopped;
static CacheState *s_state;
static volatile bool s_quit;
static volatile bool s_reading;
static volatile bool s_decoding;
static bool s_running;
static lv_timer_t *s_dispatch;

namespace {

struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

}

static bool panel_rgb888() {
    return bsp_display_get_pixel_format() == BSP_PIXEL_FORMAT_RGB888;
}

/* One window past the file header, paired with the exact byte count. The id
   only has to tell two pictures apart, and hashing all of a 500 KB cover cost
   20 ms per file; the header is skipped because encoders make it identical
   across an album. */
static uint64_t image_id_of(const CoverBytes &bytes) {
    const std::size_t size = bytes.size();
    const std::size_t from = size > kHashSkipBytes ? kHashSkipBytes : 0;
    std::size_t span = size - from;
    if (span > kHashWindowBytes) span = kHashWindowBytes;

    const uint8_t *data = bytes.data() + from;
    uint32_t hash = 0x811C9DC5u;
    for (std::size_t i = 0; i < span; i++) {
        hash ^= data[i];
        hash *= 0x01000193u;
    }
    const uint64_t id = (uint64_t)hash << 32 | (uint32_t)size;
    return id ? id : 1;
}

static void copy_text(char *out, std::size_t size, const std::string &text) {
    const std::size_t length = text.size() < size - 1 ? text.size() : size - 1;
    memcpy(out, text.data(), length);
    out[length] = '\0';
}

static void release_image_id(uint64_t id) {
    if (!id) return;
    auto it = s_state->uses.find(id);
    if (it == s_state->uses.end()) return;
    if (--it->second == 0) s_state->uses.erase(it);
}

static bool protected_entry(const ImageEntry &entry) {
    return entry.pixels.use_count() > 1 || entry.jpeg.use_count() > 1;
}

static void evict_images(ImageStore &store) {
    while (store.bytes > store.budget) {
        auto victim = store.map.end();
        uint64_t best_used = 0;
        bool best_orphan = false;
        for (auto it = store.map.begin(); it != store.map.end(); ++it) {
            if (protected_entry(it->second)) continue;
            const bool orphan = s_state->uses.find(it->first.id) == s_state->uses.end();
            if (victim == store.map.end() || (orphan && !best_orphan) ||
                (orphan == best_orphan && it->second.used < best_used)) {
                victim = it;
                best_used = it->second.used;
                best_orphan = orphan;
            }
        }
        if (victim == store.map.end()) break;
        store.bytes -= victim->second.bytes;
        store.map.erase(victim);
    }
}

static void evict_meta() {
    while (s_state->meta.size() > kMetaEntries) {
        auto victim = s_state->meta.end();
        for (auto it = s_state->meta.begin(); it != s_state->meta.end(); ++it) {
            if (victim == s_state->meta.end() || it->second.used < victim->second.used) victim = it;
        }
        if (victim == s_state->meta.end()) break;
        release_image_id(victim->second.entry->image_id);
        s_state->meta.erase(victim);
    }
}

static void store_image(ImageStore &store, const ImageKey &key, ImageEntry entry) {
    Lock lock;
    auto it = store.map.find(key);
    if (it != store.map.end()) store.bytes -= it->second.bytes;
    entry.used = ++s_state->clock;
    store.bytes += entry.bytes;
    store.map[key] = std::move(entry);
    evict_images(store);
}

static std::shared_ptr<const CoverPixels> take_pixels(const ImageKey &key) {
    auto raw = s_state->raw.map.find(key);
    if (raw != s_state->raw.map.end()) {
        raw->second.used = ++s_state->clock;
        return raw->second.pixels;
    }
    auto decoded = s_state->decoded.map.find(key);
    if (decoded != s_state->decoded.map.end() && decoded->second.rgb888 == panel_rgb888()) {
        decoded->second.used = ++s_state->clock;
        return decoded->second.pixels;
    }
    return nullptr;
}

static std::shared_ptr<MediaEntry> find_meta(const std::string &path) {
    auto it = s_state->meta.find(PsramString(path.c_str()));
    if (it == s_state->meta.end()) return nullptr;
    it->second.used = ++s_state->clock;
    return it->second.entry;
}

static bool produce(uint64_t id, int32_t side, const CoverBytes &bytes) {
    const bool rgb888 = side > kRawMaxSide && panel_rgb888();
    auto pixels = artwork_decode(bytes.data(), bytes.size(), side, rgb888);
    if (!pixels) return false;

    const ImageKey key = { id, side };
    if (side <= kRawMaxSide) {
        store_image(s_state->raw, key, { pixels, nullptr, pixels->bytes, 0, rgb888 });
        return true;
    }

    auto jpeg = psram_make_shared<JpegBytes>();
    if (artwork_encode(*pixels, jpeg.get())) {
        store_image(s_state->jpeg, key, { nullptr, jpeg, jpeg->size(), 0, false });
    }
    store_image(s_state->decoded, key, { pixels, nullptr, pixels->bytes, 0, rgb888 });
    return true;
}

static void forget_cover(const std::shared_ptr<MediaEntry> &entry) {
    Lock lock;
    release_image_id(entry->image_id);
    entry->has_cover = false;
    entry->cover_scanned = true;
    entry->image_id = 0;
}

static std::shared_ptr<MediaEntry> make_entry(const MediaSummary &summary, bool ok,
                                              const CoverArt &cover) {
    auto entry = psram_make_shared<MediaEntry>();
    entry->ok = ok;
    entry->duration_us = summary.duration_us;
    entry->audio_codec = summary.audio.codec;
    entry->sample_rate = summary.audio.sample_rate;
    entry->bitrate_bps = summary.audio.bitrate_bps;
    entry->channels = summary.audio.channels;
    entry->bits = summary.audio.bits;
    copy_text(entry->title, sizeof(entry->title), summary.tags.title);
    copy_text(entry->artist, sizeof(entry->artist), summary.tags.artist);
    copy_text(entry->album, sizeof(entry->album), summary.tags.album);
    entry->cover_at = summary.cover_at;
    entry->cover_scanned = summary.cover_scanned;
    if (cover) {
        entry->image_id = image_id_of(*cover.data);
        entry->has_cover = true;
    } else if (summary.cover_at) {
        entry->has_cover = true;
    }
    return entry;
}

/* The picture, from wherever it is cheapest: the recorded location when the
   container gave one, otherwise a full probe that reads the tag again. */
static CoverArt fetch_cover(const std::string &path, const std::shared_ptr<MediaEntry> &entry) {
    if (entry->cover_at) {
        CoverArt cover = media_probe_cover(path, s_arena, entry->cover_at);
        if (cover) return cover;
    }
    MediaSummary summary;
    std::string error;
    if (!media_probe(path, s_arena, true, &summary, &error)) return {};
    {
        Lock lock;
        entry->cover_scanned = true;
    }
    return summary.cover;
}

/* Completes an entry whose probe never saw the picture: the id is what the
   image stores are keyed on and what media_cache_image() needs to answer. */
static void adopt_image_id(const std::shared_ptr<MediaEntry> &entry, const CoverBytes &bytes) {
    Lock lock;
    entry->has_cover = true;
    entry->cover_scanned = true;
    if (entry->image_id) return;
    entry->image_id = image_id_of(bytes);
    s_state->uses[entry->image_id]++;
}

/* Stage one: everything that reads the card. Returns whether it left a job for
   the decoder. */
static bool prepare(const Request &request, DecodeJob *job) {
    const std::string path(request.path.c_str());
    const bool want_image = (request.want & MetaWantImage) && request.side > 0;

    std::shared_ptr<MediaEntry> entry;
    {
        Lock lock;
        entry = find_meta(path);
    }

    CoverArt cover;
    if (!entry) {
        MediaSummary summary;
        std::string error;
        const bool ok = media_probe(path, s_arena, want_image, &summary, &error);
        cover = summary.cover;
        entry = make_entry(summary, ok, cover);

        Lock lock;
        auto it = s_state->meta.find(request.path);
        if (it != s_state->meta.end()) release_image_id(it->second.entry->image_id);
        s_state->meta[request.path] = { entry, ++s_state->clock };
        if (entry->image_id) s_state->uses[entry->image_id]++;
        evict_meta();
    }

    if (!want_image) return false;
    /* A probe told to skip pictures leaves "no cover" unanswered unless the
       container said where one was, so only a scanned entry can say no. */
    if (!entry->has_cover && entry->cover_scanned) return false;

    if (!entry->image_id) {
        if (!cover) cover = fetch_cover(path, entry);
        if (!cover) {
            forget_cover(entry);
            return false;
        }
        adopt_image_id(entry, *cover.data);
    }

    const ImageKey key = { entry->image_id, request.side };
    std::shared_ptr<JpegBytes> jpeg;
    {
        Lock lock;
        if (take_pixels(key)) return false;
        auto it = s_state->jpeg.map.find(key);
        if (it != s_state->jpeg.map.end()) {
            it->second.used = ++s_state->clock;
            jpeg = it->second.jpeg;
        }
    }
    if (!jpeg && !cover) {
        cover = fetch_cover(path, entry);
        if (!cover) {
            forget_cover(entry);
            return false;
        }
    }

    job->path = request.path;
    job->entry = entry;
    job->cover = std::move(cover);
    job->jpeg = std::move(jpeg);
    job->key = key;
    job->token = request.token;
    return true;
}

/* Stage two: no file access, so it runs while the reader is on the next one. */
static void finish(DecodeJob &job) {
    if (job.jpeg) {
        const bool rgb888 = panel_rgb888();
        auto pixels = artwork_decode(job.jpeg->data(), job.jpeg->size(), job.key.side, rgb888);
        if (pixels) {
            store_image(s_state->decoded, job.key, { pixels, nullptr, pixels->bytes, 0, rgb888 });
            return;
        }
        /* A re-encoded copy that will not decode is worse than none: drop it so
           the next request goes back to the original picture. */
        Lock lock;
        auto it = s_state->jpeg.map.find(job.key);
        if (it != s_state->jpeg.map.end()) {
            s_state->jpeg.bytes -= it->second.bytes;
            s_state->jpeg.map.erase(it);
        }
        return;
    }
    if (!job.cover || !produce(job.key.id, job.key.side, *job.cover.data)) forget_cover(job.entry);
}

static bool take_request(Request *out) {
    Lock lock;
    for (auto &queue : s_state->queues) {
        if (queue.empty()) continue;
        *out = std::move(queue.front());
        queue.pop_front();
        return true;
    }
    return false;
}

static void exit_task() {
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

/* Reading the card and decoding a picture share nothing, so they run as two
   stages: while the decoder works on one cover the reader is already pulling
   the next file's tags. */
static void reader_task(void *) {
    while (!s_quit) {
        Request request;
        if (!take_request(&request)) {
            xSemaphoreTake(s_wake, portMAX_DELAY);
            continue;
        }

        s_reading = true;
        DecodeJob job;
        const bool decode = prepare(request, &job);
        s_reading = false;
        if (s_quit) break;

        if (decode) {
            while (xSemaphoreTake(s_decode_room, pdMS_TO_TICKS(50)) != pdTRUE) {
                if (s_quit) break;
            }
            if (s_quit) break;
            {
                Lock lock;
                s_state->decodes.push_back(std::move(job));
            }
            xSemaphoreGive(s_decode_wake);
        } else {
            Lock lock;
            s_state->completed.push_back(request.path);
        }
        if (player_status().state == PlayerState::Playing) {
            vTaskDelay(pdMS_TO_TICKS(kPlayingGapMs));
        }
    }

    xSemaphoreGive(s_stopped);
    exit_task();
}

static void decoder_task(void *) {
    while (!s_quit) {
        DecodeJob job;
        bool have = false;
        {
            Lock lock;
            if (!s_state->decodes.empty()) {
                job = std::move(s_state->decodes.front());
                s_state->decodes.pop_front();
                have = true;
            }
        }
        if (!have) {
            xSemaphoreTake(s_decode_wake, portMAX_DELAY);
            continue;
        }
        xSemaphoreGive(s_decode_room);

        s_decoding = true;
        finish(job);
        s_decoding = false;
        if (s_quit) break;
        {
            Lock lock;
            s_state->completed.push_back(job.path);
        }
    }

    artwork_codec_close();
    xSemaphoreGive(s_decoder_stopped);
    exit_task();
}

static void dispatch(lv_timer_t *) {
    PsramVector<PsramString> done;
    PsramVector<Observer> observers;
    {
        Lock lock;
        done.swap(s_state->completed);
        observers = s_state->observers;
    }
    for (const PsramString &path : done) {
        const std::string copy(path.c_str());
        for (const Observer &observer : observers) observer.on_ready(copy);
    }
}

void media_cache_init(const media_arena_t &arena) {
    if (s_lock) return;
    void *memory = heap_caps_malloc(sizeof(CacheState), MALLOC_CAP_SPIRAM);
    if (!memory) {
        ESP_LOGE(TAG, "no memory for the cache");
        return;
    }
    s_state = new (memory) CacheState();
    s_state->raw.budget = kRawBudget;
    s_state->jpeg.budget = kJpegBudget;
    s_state->decoded.budget = kDecodedBudget;
    s_arena = arena;
    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    s_decode_wake = xSemaphoreCreateBinary();
    s_decode_room = xSemaphoreCreateCounting(kDecodeDepth, kDecodeDepth);
    s_stopped = xSemaphoreCreateBinary();
    s_decoder_stopped = xSemaphoreCreateBinary();
}

static BaseType_t spawn(TaskFunction_t entry, const char *name) {
#ifdef ESP_PLATFORM
    return xTaskCreatePinnedToCoreWithCaps(entry, name, kWorkerStackBytes, nullptr, 2, nullptr, 0,
                                           MALLOC_CAP_SPIRAM);
#else
    return xTaskCreatePinnedToCore(entry, name, kWorkerStackBytes, nullptr, 2, nullptr, 0);
#endif
}

void media_cache_start() {
    if (!s_lock || s_running || !s_arena.data) return;
    s_quit = false;
    xSemaphoreTake(s_stopped, 0);
    xSemaphoreTake(s_decoder_stopped, 0);
    if (spawn(reader_task, "media_meta") != pdPASS) {
        ESP_LOGE(TAG, "no memory for the worker task");
        return;
    }
    if (spawn(decoder_task, "media_art") != pdPASS) {
        ESP_LOGE(TAG, "no memory for the decoder task");
        s_quit = true;
        xSemaphoreGive(s_wake);
        xSemaphoreTake(s_stopped, pdMS_TO_TICKS(kStopTimeoutMs));
        return;
    }
    s_running = true;
    s_dispatch = lv_timer_create(dispatch, kDispatchPeriodMs, nullptr);
}

void media_cache_stop() {
    if (!s_running) return;
    s_quit = true;
    xSemaphoreGive(s_wake);
    xSemaphoreGive(s_decode_wake);
    xSemaphoreGive(s_decode_room);
    if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(kStopTimeoutMs)) != pdTRUE) {
        ESP_LOGE(TAG, "worker did not stop");
    }
    if (xSemaphoreTake(s_decoder_stopped, pdMS_TO_TICKS(kStopTimeoutMs)) != pdTRUE) {
        ESP_LOGE(TAG, "decoder did not stop");
    }
    s_running = false;
    if (s_dispatch) {
        lv_timer_delete(s_dispatch);
        s_dispatch = nullptr;
    }

    Lock lock;
    for (auto &queue : s_state->queues) queue.clear();
    s_state->decodes.clear();
    s_state->completed.clear();
    for (ImageStore *store : { &s_state->raw, &s_state->jpeg, &s_state->decoded }) {
        store->map.clear();
        store->bytes = 0;
    }
}

uint32_t media_cache_token() {
    if (!s_lock) return 0;
    Lock lock;
    return s_state->next_token++;
}

void media_cache_observe(uint32_t token, void (*on_ready)(const std::string &path)) {
    if (!s_lock) return;
    media_cache_unobserve(token);
    Lock lock;
    s_state->observers.push_back({ token, on_ready });
}

void media_cache_unobserve(uint32_t token) {
    if (!s_lock) return;
    Lock lock;
    for (auto it = s_state->observers.begin(); it != s_state->observers.end(); ++it) {
        if (it->token != token) continue;
        s_state->observers.erase(it);
        return;
    }
}

std::shared_ptr<const MediaEntry> media_cache_lookup(const std::string &path) {
    if (!s_lock) return nullptr;
    Lock lock;
    return find_meta(path);
}

std::shared_ptr<const CoverPixels> media_cache_image(const std::string &path, int32_t side) {
    if (!s_lock || side <= 0) return nullptr;
    Lock lock;
    auto entry = find_meta(path);
    if (!entry || !entry->has_cover) return nullptr;
    return take_pixels({ entry->image_id, side });
}

void media_cache_request(const std::string &path, uint8_t want, int32_t side,
                         MetaPriority priority, uint32_t token) {
    if (!s_running) return;
    {
        Lock lock;
        PsramDeque<Request> &queue = s_state->queues[(int)priority];
        for (const Request &pending : queue) {
            if (pending.want == want && pending.side == side &&
                strcmp(pending.path.c_str(), path.c_str()) == 0) {
                return;
            }
        }
        queue.push_back({ PsramString(path.c_str()), want, side, token });
    }
    xSemaphoreGive(s_wake);
}

std::shared_ptr<const MediaEntry> media_cache_resolve(const std::string &path, uint8_t want,
                                                      int32_t side, uint32_t timeout_ms) {
    if (!s_lock) return nullptr;
    auto satisfied = [&]() -> std::shared_ptr<const MediaEntry> {
        Lock lock;
        auto entry = find_meta(path);
        if (!entry) return nullptr;
        if (!(want & MetaWantImage) || !entry->has_cover || side <= 0) return entry;
        return take_pixels({ entry->image_id, side }) ? entry : nullptr;
    };

    if (auto ready = satisfied()) return ready;
    if (!s_running) return nullptr;

    media_cache_request(path, want, side, MetaPriority::Blocking, 0);
    const int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (auto ready = satisfied()) return ready;
    }
    return media_cache_lookup(path);
}

void media_cache_idle_cancel() {
    if (!s_lock) return;
    Lock lock;
    s_state->queues[(int)MetaPriority::Idle].clear();
}

void media_cache_cancel(uint32_t token) {
    if (!s_lock || !token) return;
    uint32_t dropped = 0;
    {
        Lock lock;
        for (auto &queue : s_state->queues) {
            for (auto it = queue.begin(); it != queue.end();) {
                it = it->token == token ? queue.erase(it) : it + 1;
            }
        }
        for (auto it = s_state->decodes.begin(); it != s_state->decodes.end();) {
            if (it->token != token) {
                ++it;
                continue;
            }
            it = s_state->decodes.erase(it);
            dropped++;
        }
    }
    while (dropped--) xSemaphoreGive(s_decode_room);
}

void media_cache_forget(const std::string &mount_point) {
    if (!s_lock) return;
    Lock lock;
    for (auto it = s_state->meta.begin(); it != s_state->meta.end();) {
        if (path_is_under(it->first.c_str(), mount_point)) {
            release_image_id(it->second.entry->image_id);
            it = s_state->meta.erase(it);
        } else {
            ++it;
        }
    }
    for (auto &queue : s_state->queues) {
        for (auto it = queue.begin(); it != queue.end();) {
            it = path_is_under(it->path.c_str(), mount_point) ? queue.erase(it) : it + 1;
        }
    }
}

void media_cache_invalidate_decoded() {
    if (!s_lock) return;
    Lock lock;
    s_state->decoded.map.clear();
    s_state->decoded.bytes = 0;
}

#ifdef CONFIG_HARNESS

static bool harness_command(int argc, const char *const *argv, void *) {
    if (!s_lock) return false;
    if (argc >= 2 && strcmp(argv[1], "drain") == 0) {
        for (int i = 0; i < 600; i++) {
            {
                Lock lock;
                bool pending = !s_state->completed.empty() || s_reading || s_decoding ||
                               !s_state->decodes.empty();
                for (const auto &queue : s_state->queues) pending = pending || !queue.empty();
                if (!pending) break;
            }
            vTaskDelay(pdMS_TO_TICKS(50));
        }
        harness_reply("OK meta drain");
        return true;
    }
    if (argc >= 2 && strcmp(argv[1], "stats") == 0) {
        Lock lock;
        ESP_LOGI(TAG, "meta %u images %u raw %u/%u jpeg %u/%u decoded %u/%u",
                 (unsigned)s_state->meta.size(), (unsigned)s_state->uses.size(),
                 (unsigned)s_state->raw.map.size(), (unsigned)s_state->raw.bytes,
                 (unsigned)s_state->jpeg.map.size(), (unsigned)s_state->jpeg.bytes,
                 (unsigned)s_state->decoded.map.size(), (unsigned)s_state->decoded.bytes);
        return true;
    }
    return false;
}

void media_cache_register_harness() {
    harness_register("meta", harness_command, nullptr);
}

#else

void media_cache_register_harness() {}

#endif
