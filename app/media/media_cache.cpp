/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "sdkconfig.h"

#include "media_cache.hpp"
#include "media/demuxer.hpp"
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

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

static const char *TAG = "media_cache";

static constexpr std::size_t kMetaEntries = 512;
static constexpr std::size_t kRawBudget = 1024 * 1024;
static constexpr std::size_t kJpegBudget = 4 * 1024 * 1024;
static constexpr std::size_t kDecodedBudget = 2 * 1024 * 1024;
static constexpr std::size_t kViewBudget = 6 * 1024 * 1024;
static constexpr int32_t kRawMaxSide = 128;
static constexpr int32_t kDecodedMaxSide = 640;
static constexpr uint32_t kReaderStackBytes = 8192;
/* The decoder runs the image decoders themselves, not just a demuxer's header
   walk, so it gets more room than the reader. */
static constexpr uint32_t kDecoderStackBytes = 16384;
static constexpr uint32_t kDispatchPeriodMs = 100;
static constexpr uint32_t kPlayingGapMs = 200;
static constexpr uint32_t kStopTimeoutMs = 2000;
static constexpr std::size_t kHashSkipBytes = 1024;
static constexpr std::size_t kHashWindowBytes = 8 * 1024;
static constexpr std::size_t kImageProbeWindow = 128 * 1024;
static constexpr std::size_t kImageChunkBytes = 256 * 1024;
static constexpr std::size_t kImageAlignment = 64;
/* One cover queued for the decoder while the reader fetches the next: enough
   to hide the decode behind the SD read without holding a third picture. */
static constexpr uint32_t kDecodeDepth = 1;

namespace {

struct ImageKey {
    uint64_t id;
    ImageBox box;

    bool operator==(const ImageKey &other) const {
        return id == other.id && box == other.box;
    }
};

struct ImageKeyHash {
    std::size_t operator()(const ImageKey &key) const {
        const uint64_t box = ((uint64_t)(uint16_t)key.box.width << 16) | (uint16_t)key.box.height;
        return (std::size_t)(key.id ^ (box * 0x9E3779B97F4A7C15ull));
    }
};

using JpegBytes = PsramVector<uint8_t>;

struct ImageEntry {
    std::shared_ptr<ImagePixels> pixels;
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
    ImageBox box;
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
    bool image = false;
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
    ImageStore view;
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
/* Whoever owns the running job may withdraw it: media_cache_cancel() raises the
   flag of the stage whose job carries the cancelled token. */
static volatile bool s_reader_cancel;
static volatile bool s_decoder_cancel;
static uint32_t s_reader_token;
static uint32_t s_decoder_token;
static bool s_running;
static lv_timer_t *s_dispatch;
static TaskHandle_t s_reader_task;
static TaskHandle_t s_decoder_task;

namespace {

struct Lock {
    Lock() { xSemaphoreTake(s_lock, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(s_lock); }
};

}

static unsigned psram_free() {
#ifdef ESP_PLATFORM
    return (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
#else
    return 0;
#endif
}

static unsigned stack_left(TaskHandle_t task = nullptr) {
#ifdef ESP_PLATFORM
    return (unsigned)uxTaskGetStackHighWaterMark(task);
#else
    (void)task;
    return 0;
#endif
}

static bool panel_rgb888() {
    return bsp_display_get_pixel_format() == BSP_PIXEL_FORMAT_RGB888;
}

/* One window past the file header, paired with the exact byte count. The id
   only has to tell two pictures apart, and hashing all of a 500 KB cover cost
   20 ms per file; the header is skipped because encoders make it identical
   across an album. */
static uint64_t image_id_of(const uint8_t *data, std::size_t window, uint64_t size) {
    const std::size_t from = window > kHashSkipBytes ? kHashSkipBytes : 0;
    std::size_t span = window - from;
    if (span > kHashWindowBytes) span = kHashWindowBytes;

    uint32_t hash = 0x811C9DC5u;
    for (std::size_t i = 0; i < span; i++) {
        hash ^= data[from + i];
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

/* Which store a size belongs in: thumbnails are kept as pixels, artwork and
   the picture on screen are re-encoded, and the screen-sized ones get their own
   budget so that opening a folder of photographs cannot evict every thumbnail. */
static ImageStore &pixel_store(const ImageKey &key) {
    if (key.box.longest() <= kRawMaxSide) return s_state->raw;
    return key.box.longest() <= kDecodedMaxSide ? s_state->decoded : s_state->view;
}

static std::shared_ptr<const ImagePixels> take_pixels(const ImageKey &key) {
    auto raw = s_state->raw.map.find(key);
    if (raw != s_state->raw.map.end()) {
        raw->second.used = ++s_state->clock;
        return raw->second.pixels;
    }
    for (ImageStore *store : { &s_state->decoded, &s_state->view }) {
        auto it = store->map.find(key);
        if (it != store->map.end() && it->second.rgb888 == panel_rgb888()) {
            it->second.used = ++s_state->clock;
            return it->second.pixels;
        }
    }
    return nullptr;
}

static std::shared_ptr<MediaEntry> find_meta(const std::string &path) {
    auto it = s_state->meta.find(PsramString(path.c_str()));
    if (it == s_state->meta.end()) return nullptr;
    it->second.used = ++s_state->clock;
    return it->second.entry;
}

static void keep_meta(const PsramString &path, const std::shared_ptr<MediaEntry> &entry) {
    Lock lock;
    auto it = s_state->meta.find(path);
    if (it != s_state->meta.end()) release_image_id(it->second.entry->image_id);
    s_state->meta[path] = { entry, ++s_state->clock };
    if (entry->image_id) s_state->uses[entry->image_id]++;
    evict_meta();
}

static void store_pixels(const ImageKey &key, std::shared_ptr<ImagePixels> pixels, bool rgb888) {
    if (key.box.longest() <= kRawMaxSide) {
        store_image(s_state->raw, key, { pixels, nullptr, pixels->bytes, 0, rgb888 });
        return;
    }
    auto jpeg = psram_make_shared<JpegBytes>();
    if (image_encode(*pixels, jpeg.get())) {
        store_image(s_state->jpeg, key, { nullptr, jpeg, jpeg->size(), 0, false });
    }
    store_image(pixel_store(key), key, { pixels, nullptr, pixels->bytes, 0, rgb888 });
}

static bool produce(const ImageKey &key, const uint8_t *data, std::size_t size) {
    const bool rgb888 = key.box.longest() > kRawMaxSide && panel_rgb888();
    auto pixels = image_decode(data, size, key.box, rgb888, &s_decoder_cancel);
    if (!pixels) return false;
    store_pixels(key, std::move(pixels), rgb888);
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
    entry->file_bytes = summary.file_bytes;
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
        entry->image_id = image_id_of(cover.data->data(), cover.data->size(), cover.data->size());
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
    entry->image_id = image_id_of(bytes.data(), bytes.size(), bytes.size());
    s_state->uses[entry->image_id]++;
}

static CoverArt read_image(const std::string &path, std::size_t limit, int64_t *file_bytes) {
    CoverArt cover;
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) {
        ESP_LOGE(TAG, "cannot open %s", path.c_str());
        return cover;
    }
    fseek(fp, 0, SEEK_END);
    const long size = ftell(fp);
    rewind(fp);
    if (size <= 0) {
        ESP_LOGE(TAG, "empty file %s", path.c_str());
        fclose(fp);
        return cover;
    }
    *file_bytes = size;

    const std::size_t want = std::min((std::size_t)size, limit);
    auto *buffer = static_cast<uint8_t *>(heap_caps_aligned_alloc(
        kImageAlignment, (want + kImageAlignment - 1) / kImageAlignment * kImageAlignment,
        MALLOC_CAP_SPIRAM));
    if (!buffer) {
        ESP_LOGE(TAG, "no memory to read %u bytes of %s (psram largest %u)", (unsigned)want,
                 path.c_str(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        fclose(fp);
        return cover;
    }
    for (std::size_t done = 0; done < want;) {
        const std::size_t chunk = std::min(kImageChunkBytes, want - done);
        if (s_reader_cancel) {
            heap_caps_free(buffer);
            fclose(fp);
            return cover;
        }
        if (fread(buffer + done, 1, chunk, fp) != chunk) {
            ESP_LOGE(TAG, "short read at %u of %u bytes in %s", (unsigned)done, (unsigned)want,
                     path.c_str());
            heap_caps_free(buffer);
            fclose(fp);
            return cover;
        }
        done += chunk;
    }
    fclose(fp);
    cover.data = psram_make_shared<CoverBytes>(buffer, buffer, want);
    cover.format = CoverFormat::Jpeg;
    return cover;
}

/* An image file is its own picture: the header gives the size to show in Media
   Info, and the same window that is hashed for a cover art id is hashed here,
   so two copies of one photograph share their decoded pixels. */
static std::shared_ptr<MediaEntry> probe_image(const std::string &path) {
    int64_t size = 0;
    CoverArt data = read_image(path, kImageProbeWindow, &size);
    auto entry = psram_make_shared<MediaEntry>();
    entry->file_bytes = size;
    entry->cover_scanned = true;
    if (!data) return entry;

    ImageHeader header;
    /* The frame header may sit past the window -- a camera JPEG carries tens of
       KB of EXIF and an embedded thumbnail first -- and that says nothing about
       whether the file can be decoded, so only the format has to be recognised
       here. The size is filled in by the decode when the probe could not read
       it. */
    if (!image_header(data.data->data(), data.data->size(), &header) &&
        header.format == ImageFormat::Unknown) {
        ESP_LOGE(TAG, "not a JPEG or PNG: %s (%u bytes)", path.c_str(), (unsigned)size);
        return entry;
    }
    entry->ok = true;
    entry->has_cover = true;
    entry->image_format = header.format;
    entry->image_width = (uint16_t)header.width;
    entry->image_height = (uint16_t)header.height;
    entry->image_baseline = header.hardware;
    entry->image_id = image_id_of(data.data->data(), data.data->size(), (uint64_t)size);

    ImageExif exif;
    if (image_exif_parse(data.data->data(), data.data->size(), &exif)) {
        entry->image_exif = psram_make_shared<ImageExif>(exif);
    }
    return entry;
}

static bool prepare_image(const Request &request, const std::string &path, DecodeJob *job) {
    const bool want_image = (request.want & MetaWantImage) && request.box.valid();

    std::shared_ptr<MediaEntry> entry;
    {
        Lock lock;
        entry = find_meta(path);
    }

    if (!entry) {
        entry = probe_image(path);
        if (s_reader_cancel) return false;
        keep_meta(request.path, entry);
    }
    if (!want_image || !entry->ok || entry->image_too_large) return false;

    const ImageKey key = { entry->image_id, request.box };
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

    job->path = request.path;
    job->entry = entry;
    job->jpeg = std::move(jpeg);
    job->key = key;
    job->token = request.token;
    job->image = true;
    return true;
}

/* Stage one: everything that reads the card. Returns whether it left a job for
   the decoder. */
static bool prepare(const Request &request, DecodeJob *job) {
    const std::string path(request.path.c_str());
    if (demuxer_media_kind(path.c_str()) == MediaKind::Image) {
        return prepare_image(request, path, job);
    }

    const bool want_image = (request.want & MetaWantImage) && request.box.valid();

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
        keep_meta(request.path, entry);
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

    const ImageKey key = { entry->image_id, request.box };
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

/* Stage two: no file access for tags and cover art, so it runs while the reader
   is on the next one. An image file is read here instead, which gives up that
   overlap to keep one encoded picture in memory at a time. */
static void finish(DecodeJob &job) {
    if (job.jpeg) {
        const bool rgb888 = panel_rgb888();
        auto pixels = image_decode(job.jpeg->data(), job.jpeg->size(), job.key.box, rgb888,
                                   &s_decoder_cancel);
        if (pixels) {
            store_image(pixel_store(job.key), job.key,
                        { pixels, nullptr, pixels->bytes, 0, rgb888 });
            return;
        }
        if (s_decoder_cancel) return;
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

    bool produced = false;
    if (job.cover) {
        produced = produce(job.key, job.cover.data->data(), job.cover.data->size());
    } else if (job.image) {
        const bool rgb888 = panel_rgb888();
        ImageNotes notes;
        auto pixels = image_decode_file(std::string(job.path.c_str()), job.key.box, rgb888,
                                        &s_decoder_cancel, &notes);
        produced = pixels != nullptr;
        /* The header the decode saw is the whole file's, so it knows the size
           even when the probe's window stopped short of the frame header. */
        {
            Lock lock;
            if (notes.header.width) {
                job.entry->image_width = (uint16_t)notes.header.width;
                job.entry->image_height = (uint16_t)notes.header.height;
                job.entry->image_format = notes.header.format;
                job.entry->image_baseline = notes.header.hardware;
            }
            /* Per file, not per box: a picture that ran out of memory at one
               size runs out at every other one too. A failure is recorded
               rather than taken out of `has_cover`, so the sizes that did
               decode stay reachable through the entry's id. */
            job.entry->image_failed = !produced && !s_decoder_cancel;
            job.entry->image_too_large = notes.out_of_memory;
        }
        if (produced) store_pixels(job.key, std::move(pixels), rgb888);
    }
    if (!produced && !s_decoder_cancel) {
        ESP_LOGE(TAG, "no picture for %s at %dx%d (%u source bytes, psram free %u, stack left %u)",
                 job.path.c_str(), job.key.box.width, job.key.box.height,
                 (unsigned)job.entry->file_bytes, psram_free(), stack_left());
        if (!job.image) forget_cover(job.entry);
    }
}

static bool take_request(Request *out) {
    Lock lock;
    for (auto &queue : s_state->queues) {
        if (queue.empty()) continue;
        *out = std::move(queue.front());
        queue.pop_front();
        s_reader_cancel = false;
        s_reader_token = out->token;
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
        const bool cancelled = s_reader_cancel;
        s_reading = false;
        {
            Lock lock;
            s_reader_token = 0;
        }
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
        } else if (!cancelled) {
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
                s_decoder_cancel = false;
                s_decoder_token = job.token;
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
        const bool cancelled = s_decoder_cancel;
        s_decoding = false;
        {
            Lock lock;
            s_decoder_token = 0;
        }
        if (s_quit) break;
        if (cancelled) continue;
        {
            Lock lock;
            s_state->completed.push_back(job.path);
        }
    }

    image_codec_close();
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
    s_state->view.budget = kViewBudget;
    s_arena = arena;
    s_lock = xSemaphoreCreateMutex();
    s_wake = xSemaphoreCreateBinary();
    s_decode_wake = xSemaphoreCreateBinary();
    s_decode_room = xSemaphoreCreateCounting(kDecodeDepth, kDecodeDepth);
    s_stopped = xSemaphoreCreateBinary();
    s_decoder_stopped = xSemaphoreCreateBinary();
}

static BaseType_t spawn(TaskFunction_t entry, const char *name, uint32_t stack,
                        TaskHandle_t *handle) {
#ifdef ESP_PLATFORM
    return xTaskCreatePinnedToCoreWithCaps(entry, name, stack, nullptr, 2, handle, 0,
                                           MALLOC_CAP_SPIRAM);
#else
    return xTaskCreatePinnedToCore(entry, name, stack, nullptr, 2, handle, 0);
#endif
}

void media_cache_start() {
    if (!s_lock || s_running || !s_arena.data) return;
    s_quit = false;
    s_reader_cancel = false;
    s_decoder_cancel = false;
    s_reader_token = 0;
    s_decoder_token = 0;
    xSemaphoreTake(s_stopped, 0);
    xSemaphoreTake(s_decoder_stopped, 0);
    if (spawn(reader_task, "media_meta", kReaderStackBytes, &s_reader_task) != pdPASS) {
        ESP_LOGE(TAG, "no memory for the worker task");
        return;
    }
    if (spawn(decoder_task, "media_art", kDecoderStackBytes, &s_decoder_task) != pdPASS) {
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
    s_reader_cancel = true;
    s_decoder_cancel = true;
    xSemaphoreGive(s_decode_room);
    for (;;) {
        xSemaphoreGive(s_wake);
        if (xSemaphoreTake(s_stopped, pdMS_TO_TICKS(kStopTimeoutMs)) == pdTRUE) break;
        ESP_LOGE(TAG, "worker did not stop");
    }
    for (;;) {
        xSemaphoreGive(s_decode_wake);
        if (xSemaphoreTake(s_decoder_stopped, pdMS_TO_TICKS(kStopTimeoutMs)) == pdTRUE) break;
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
    for (ImageStore *store : { &s_state->raw, &s_state->jpeg, &s_state->decoded, &s_state->view }) {
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

std::shared_ptr<const ImagePixels> media_cache_image(const std::string &path, ImageBox box) {
    if (!s_lock || !box.valid()) return nullptr;
    Lock lock;
    auto entry = find_meta(path);
    if (!entry || !entry->has_cover) return nullptr;
    return take_pixels({ entry->image_id, box });
}

void media_cache_request(const std::string &path, uint8_t want, ImageBox box,
                         MetaPriority priority, uint32_t token) {
    if (!s_running) return;
    {
        Lock lock;
        PsramDeque<Request> &queue = s_state->queues[(int)priority];
        for (const Request &pending : queue) {
            if (pending.want == want && pending.box == box &&
                strcmp(pending.path.c_str(), path.c_str()) == 0) {
                return;
            }
        }
        queue.push_back({ PsramString(path.c_str()), want, box, token });
    }
    xSemaphoreGive(s_wake);
}

std::shared_ptr<const MediaEntry> media_cache_resolve(const std::string &path, uint8_t want,
                                                      ImageBox box, uint32_t timeout_ms) {
    if (!s_lock) return nullptr;
    auto satisfied = [&]() -> std::shared_ptr<const MediaEntry> {
        Lock lock;
        auto entry = find_meta(path);
        if (!entry) return nullptr;
        if (!(want & MetaWantImage) || !entry->has_cover || !box.valid()) return entry;
        return take_pixels({ entry->image_id, box }) ? entry : nullptr;
    };

    if (auto ready = satisfied()) return ready;
    if (!s_running) return nullptr;

    media_cache_request(path, want, box, MetaPriority::Blocking, 0);
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
        if (s_reader_token == token) s_reader_cancel = true;
        if (s_decoder_token == token) s_decoder_cancel = true;
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
    for (ImageStore *store : { &s_state->decoded, &s_state->view }) {
        store->map.clear();
        store->bytes = 0;
    }
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
        ESP_LOGI(TAG,
                 "meta %u images %u raw %u/%u jpeg %u/%u decoded %u/%u view %u/%u psram %u/%u",
                 (unsigned)s_state->meta.size(), (unsigned)s_state->uses.size(),
                 (unsigned)s_state->raw.map.size(), (unsigned)s_state->raw.bytes,
                 (unsigned)s_state->jpeg.map.size(), (unsigned)s_state->jpeg.bytes,
                 (unsigned)s_state->decoded.map.size(), (unsigned)s_state->decoded.bytes,
                 (unsigned)s_state->view.map.size(), (unsigned)s_state->view.bytes, psram_free(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        ESP_LOGI(TAG, "stack left: reader %u decoder %u", stack_left(s_reader_task),
                 stack_left(s_decoder_task));
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
