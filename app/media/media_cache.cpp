/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "sdkconfig.h"

#include "media_cache.hpp"
#include "media/cache_heap.hpp"
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
#include <atomic>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <utility>

static const char *TAG = "media_cache";

static constexpr std::size_t kMetaEntries = 512;
static constexpr std::size_t kMetaHeapBytes = 768 * 1024;
static constexpr std::size_t kThumbnailHeapBytes = 2 * 1024 * 1024;
static constexpr std::size_t kPictureHeapBytes = 4 * 1024 * 1024;
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

static CacheHeap s_meta_heap;
static CacheHeap s_thumbnail_heap;
static CacheHeap s_picture_heap;
static std::atomic<bool> s_meta_overflow;
static std::atomic<int> s_live_pictures;
static std::atomic<bool> s_release_pictures;

/* Past the meta heap the general heap takes over rather than failing: the
   containers below cannot report a failed allocation. */
static void *meta_allocate(std::size_t bytes) {
    if (void *memory = s_meta_heap.allocate(bytes, alignof(std::max_align_t))) {
        s_meta_overflow = false;
        return memory;
    }
    if (!s_meta_overflow.exchange(true)) {
        ESP_LOGW(TAG, "meta heap full, using the general heap (%u bytes free)",
                 (unsigned)s_meta_heap.free_bytes());
    }
    void *memory = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
    if (!memory) abort();
    return memory;
}

static void meta_release(void *memory) {
    if (s_meta_heap.contains(memory)) s_meta_heap.release(memory);
    else heap_caps_free(memory);
}

namespace {

template <class T>
struct MetaAllocator {
    using value_type = T;

    MetaAllocator() = default;
    template <class U> MetaAllocator(const MetaAllocator<U> &) {}

    T *allocate(std::size_t count) { return static_cast<T *>(meta_allocate(count * sizeof(T))); }
    void deallocate(T *pointer, std::size_t) { meta_release(pointer); }

    template <class U> bool operator==(const MetaAllocator<U> &) const { return true; }
    template <class U> bool operator!=(const MetaAllocator<U> &) const { return false; }
};

using MetaString = std::basic_string<char, std::char_traits<char>, MetaAllocator<char>>;
template <class T> using MetaVector = std::vector<T, MetaAllocator<T>>;
template <class T> using MetaDeque = std::deque<T, MetaAllocator<T>>;
template <class K, class V, class H = std::hash<K>>
using MetaMap = std::unordered_map<K, V, H, std::equal_to<K>, MetaAllocator<std::pair<const K, V>>>;

template <class T, class... Args>
std::shared_ptr<T> meta_make_shared(Args &&...args) {
    return std::allocate_shared<T>(MetaAllocator<T>(), std::forward<Args>(args)...);
}

struct MetaStringHash {
    std::size_t operator()(const MetaString &value) const {
        std::size_t hash = 2166136261u;
        for (char c : value) {
            hash ^= (unsigned char)c;
            hash *= 16777619u;
        }
        return hash;
    }
};

struct ImageKey {
    uint64_t id;
    ImageSize box;

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

struct PictureBytes {
    uint8_t *data = nullptr;
    std::size_t size = 0;

    PictureBytes() { s_live_pictures++; }
    PictureBytes(const PictureBytes &) = delete;
    PictureBytes &operator=(const PictureBytes &) = delete;
    ~PictureBytes();
};

struct Thumbnail {
    std::shared_ptr<ImagePixels> pixels;
    uint64_t used = 0;
};

struct Picture {
    std::shared_ptr<PictureBytes> jpeg;
    uint64_t used = 0;
};

/* Pixels a decode produced for someone waiting on them, handed over before
   they are re-encoded for the picture store. */
struct Staged {
    ImageKey key = {};
    std::shared_ptr<ImagePixels> pixels;
};

struct Request {
    MetaString path;
    uint8_t want = 0;
    ImageSize box;
    uint32_t token = 0;
    bool waited = false;
};

struct MetaSlot {
    std::shared_ptr<MediaEntry> entry;
    uint64_t used = 0;
};

struct DecodeJob {
    MetaString path;
    std::shared_ptr<MediaEntry> entry;
    CoverArt cover;
    ImageKey key;
    uint32_t token = 0;
    bool image = false;
    bool thumbnail = false;
    bool waited = false;
    bool notified = false;
    /* The idle request that started a job someone waited for later: withdrawing
       the one waiting leaves it a prefetch again. */
    uint32_t prefetch_token = 0;
};

struct Observer {
    uint32_t token;
    void (*on_ready)(const std::string &);
};

struct CacheState {
    MetaMap<MetaString, MetaSlot, MetaStringHash> meta;
    MetaMap<uint64_t, uint32_t> uses;
    MetaMap<ImageKey, Thumbnail, ImageKeyHash> thumbnails;
    MetaMap<ImageKey, Picture, ImageKeyHash> pictures;
    Staged staged;
    MetaDeque<Request> queues[3];
    MetaDeque<DecodeJob> decodes;
    MetaVector<MetaString> completed;
    MetaVector<Observer> observers;
    /* The path each stage is on, beside its token: a requester withdrawing all
       but the rows it still shows names paths, not tokens. The rest is what a
       request for the same picture needs to join the job instead of repeating
       it. */
    MetaString reader_path;
    uint8_t reader_want = 0;
    ImageSize reader_box;
    bool reader_waited = false;
    uint32_t reader_prefetch_token = 0;
    MetaString decoder_path;
    ImageKey decoder_key = {};
    bool decoder_thumbnail = false;
    bool decoder_waited = false;
    uint32_t decoder_prefetch_token = 0;
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

PictureBytes::~PictureBytes() {
    s_picture_heap.release(data);
    if (--s_live_pictures == 0 && s_release_pictures.exchange(false)) s_picture_heap.destroy();
}

/* Orphans first (no meta entry refers to the id any more), then the least
   recently used; never what someone still holds. */
template <class Store, class Held>
static bool evict_one(Store &store, Held held) {
    auto victim = store.end();
    uint64_t best_used = 0;
    bool best_orphan = false;
    for (auto it = store.begin(); it != store.end(); ++it) {
        if (held(it->second)) continue;
        const bool orphan = s_state->uses.find(it->first.id) == s_state->uses.end();
        if (victim == store.end() || (orphan && !best_orphan) ||
            (orphan == best_orphan && it->second.used < best_used)) {
            victim = it;
            best_used = it->second.used;
            best_orphan = orphan;
        }
    }
    if (victim == store.end()) return false;
    store.erase(victim);
    return true;
}

static bool held_thumbnail(const Thumbnail &thumbnail) {
    return thumbnail.pixels.use_count() > 1;
}

static bool held_picture(const Picture &picture) {
    return picture.jpeg.use_count() > 1;
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

static std::shared_ptr<const ImagePixels> take_thumbnail(const ImageKey &key) {
    auto it = s_state->thumbnails.find(key);
    if (it == s_state->thumbnails.end()) return nullptr;
    it->second.used = ++s_state->clock;
    return it->second.pixels;
}

static bool has_picture(const ImageKey &key) {
    auto it = s_state->pictures.find(key);
    if (it == s_state->pictures.end()) return false;
    it->second.used = ++s_state->clock;
    return true;
}

static bool cached(const ImageKey &key, bool thumbnail) {
    return thumbnail ? take_thumbnail(key) != nullptr : has_picture(key);
}

static std::shared_ptr<MediaEntry> find_meta(const std::string &path) {
    auto it = s_state->meta.find(MetaString(path.c_str()));
    if (it == s_state->meta.end()) return nullptr;
    it->second.used = ++s_state->clock;
    return it->second.entry;
}

static void keep_meta(const MetaString &path, const std::shared_ptr<MediaEntry> &entry) {
    Lock lock;
    auto it = s_state->meta.find(path);
    if (it != s_state->meta.end()) release_image_id(it->second.entry->image_id);
    s_state->meta[path] = { entry, ++s_state->clock };
    if (entry->image_id) s_state->uses[entry->image_id]++;
    evict_meta();
}

static void release_thumbnail(uint8_t *data) {
    s_thumbnail_heap.release(data);
}

static void store_thumbnail(const ImageKey &key, const ImagePixels &source) {
    Lock lock;
    void *memory = s_thumbnail_heap.allocate(source.bytes, kImageAlignment);
    while (!memory && evict_one(s_state->thumbnails, held_thumbnail)) {
        memory = s_thumbnail_heap.allocate(source.bytes, kImageAlignment);
    }
    if (!memory) {
        ESP_LOGW(TAG, "no room for a %ux%u thumbnail", source.width, source.height);
        return;
    }
    memcpy(memory, source.data, source.bytes);
    auto pixels = meta_make_shared<ImagePixels>();
    pixels->data = static_cast<uint8_t *>(memory);
    pixels->width = source.width;
    pixels->height = source.height;
    pixels->stride = source.stride;
    pixels->bytes = source.bytes;
    pixels->rgb888 = source.rgb888;
    pixels->release = release_thumbnail;
    s_state->thumbnails[key] = { std::move(pixels), ++s_state->clock };
}

static bool store_picture(const ImageKey &key, const uint8_t *data, std::size_t size) {
    Lock lock;
    if (!s_picture_heap.ready() || s_release_pictures) return false;
    void *memory = s_picture_heap.allocate(size);
    while (!memory && evict_one(s_state->pictures, held_picture)) {
        memory = s_picture_heap.allocate(size);
    }
    if (!memory) {
        ESP_LOGW(TAG, "no room for a %u byte picture", (unsigned)size);
        return false;
    }
    memcpy(memory, data, size);
    auto jpeg = meta_make_shared<PictureBytes>();
    jpeg->data = static_cast<uint8_t *>(memory);
    jpeg->size = size;
    s_state->pictures[key] = { std::move(jpeg), ++s_state->clock };
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
    auto entry = meta_make_shared<MediaEntry>();
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

/* The picture a camera leaves in EXIF, taken out of the window the probe has
   already read. A few KB, so it is copied rather than kept by reference: the
   window is freed as soon as the probe is done with it. */
static CoverArt keep_thumb(const uint8_t *data, std::size_t at, std::size_t bytes) {
    CoverArt thumb;
    const std::size_t span = (bytes + kImageAlignment - 1) / kImageAlignment * kImageAlignment;
    auto *buffer =
        static_cast<uint8_t *>(heap_caps_aligned_alloc(kImageAlignment, span, MALLOC_CAP_SPIRAM));
    if (!buffer) return thumb;
    memcpy(buffer, data + at, bytes);
    thumb.data = psram_make_shared<CoverBytes>(buffer, buffer, bytes);
    thumb.format = CoverFormat::Jpeg;
    return thumb;
}

/* The same picture for an entry that was probed earlier and whose pixels have
   since been evicted: a few KB off the card instead of the whole photograph. */
static CoverArt read_thumb(const std::string &path, uint32_t at, uint32_t bytes) {
    CoverArt thumb;
    FILE *fp = fopen(path.c_str(), "rb");
    if (!fp) return thumb;
    const std::size_t span = (bytes + kImageAlignment - 1) / kImageAlignment * kImageAlignment;
    auto *buffer =
        static_cast<uint8_t *>(heap_caps_aligned_alloc(kImageAlignment, span, MALLOC_CAP_SPIRAM));
    if (!buffer) {
        fclose(fp);
        return thumb;
    }
    if (fseek(fp, at, SEEK_SET) != 0 || fread(buffer, 1, bytes, fp) != bytes) {
        ESP_LOGE(TAG, "cannot read the exif thumbnail of %s", path.c_str());
        heap_caps_free(buffer);
        fclose(fp);
        return thumb;
    }
    fclose(fp);
    thumb.data = psram_make_shared<CoverBytes>(buffer, buffer, bytes);
    thumb.format = CoverFormat::Jpeg;
    return thumb;
}

/* The thumbnail only stands in for the picture when it is at least as big as
   the box: below that it would be enlarged, and the picture it replaces would
   have come out sharper. */
static bool thumb_covers(const std::shared_ptr<MediaEntry> &entry, ImageSize box) {
    const auto &exif = entry->image_exif;
    if (!exif || !exif->thumb_bytes) return false;
    return exif->thumb_width >= box.width && exif->thumb_height >= box.height;
}

/* An image file is its own picture: the header gives the size to show in Media
   Info, and the same window that is hashed for a cover art id is hashed here,
   so two copies of one photograph share their decoded pixels. */
static std::shared_ptr<MediaEntry> probe_image(const std::string &path, CoverArt *thumb) {
    int64_t size = 0;
    CoverArt data = read_image(path, kImageProbeWindow, &size);
    auto entry = meta_make_shared<MediaEntry>();
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
        entry->image_exif = meta_make_shared<ImageExif>(exif);
        if (thumb && exif.thumb_bytes) {
            *thumb = keep_thumb(data.data->data(), exif.thumb_at, exif.thumb_bytes);
        }
    }
    return entry;
}

static bool wants_pixels(const Request &request) {
    return (request.want & (MetaWantThumbnail | MetaWantImage)) && request.box.valid();
}

static bool wants_thumbnail(const Request &request) {
    return request.want & MetaWantThumbnail;
}

/* Nobody is waiting for a picture that has nowhere to be kept. */
static bool worth_decoding(const Request &request) {
    return wants_thumbnail(request) || request.waited || s_picture_heap.ready();
}

static bool prepare_image(const Request &request, const std::string &path, DecodeJob *job) {
    const bool want_image = wants_pixels(request);

    std::shared_ptr<MediaEntry> entry;
    {
        Lock lock;
        entry = find_meta(path);
    }

    CoverArt thumb;
    if (!entry) {
        entry = probe_image(path, want_image ? &thumb : nullptr);
        if (s_reader_cancel) return false;
        keep_meta(request.path, entry);
    }
    if (!want_image || !entry->ok || !worth_decoding(request)) return false;

    const ImageKey key = { entry->image_id, request.box };
    {
        Lock lock;
        if (cached(key, wants_thumbnail(request))) return false;
    }

    /* A browser-sized box is served from the EXIF thumbnail when there is one:
       a few KB and a tiny decode instead of megabytes off the card and a full
       picture through the resizer. It is also the only thing a picture too
       large to decode can still show. */
    if (!thumb_covers(entry, request.box)) {
        thumb = {};
        if (entry->image_too_large) return false;
    } else if (!thumb) {
        thumb = read_thumb(path, entry->image_exif->thumb_at, entry->image_exif->thumb_bytes);
    }
    if (s_reader_cancel) return false;

    job->path = request.path;
    job->entry = entry;
    job->cover = std::move(thumb);
    job->key = key;
    job->token = request.token;
    job->image = true;
    job->thumbnail = wants_thumbnail(request);
    job->waited = request.waited;
    return true;
}

/* Stage one: everything that reads the card. Returns whether it left a job for
   the decoder. */
static bool prepare(const Request &request, DecodeJob *job) {
    const std::string path(request.path.c_str());
    if (demuxer_media_kind(path.c_str()) == MediaKind::Image) {
        return prepare_image(request, path, job);
    }

    const bool want_image = wants_pixels(request);

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

    if (!want_image || !worth_decoding(request)) return false;
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
    {
        Lock lock;
        if (cached(key, wants_thumbnail(request))) return false;
    }
    if (!cover) {
        cover = fetch_cover(path, entry);
        if (!cover) {
            forget_cover(entry);
            return false;
        }
    }

    job->path = request.path;
    job->entry = entry;
    job->cover = std::move(cover);
    job->key = key;
    job->token = request.token;
    job->thumbnail = wants_thumbnail(request);
    job->waited = request.waited;
    return true;
}

/* Someone waiting gets the pixels as they come out of the decoder, without
   waiting for the re-encode or paying for a decode of it. */
static void hand_over(DecodeJob &job, const std::shared_ptr<ImagePixels> &pixels) {
    Lock lock;
    if (job.notified || !(job.waited || s_state->decoder_waited)) return;
    s_state->staged = { job.key, pixels };
    s_state->completed.push_back(job.path);
    job.notified = true;
}

/* Stage two: no file access for tags and cover art, so it runs while the reader
   is on the next one. An image file is read here instead, which gives up that
   overlap to keep one encoded picture in memory at a time. */
static void finish(DecodeJob &job) {
    const bool rgb888 = !job.thumbnail && panel_rgb888();
    std::shared_ptr<ImagePixels> pixels;
    if (job.cover) {
        pixels = image_decode(job.cover.data->data(), job.cover.data->size(), job.key.box, rgb888,
                              &s_decoder_cancel);
    }
    /* An EXIF thumbnail that will not decode is not the whole answer: the
       picture itself is still there, so the full path is the fallback. */
    if (!pixels && !s_decoder_cancel && job.image && !job.entry->image_too_large) {
        ImageNotes notes;
        pixels = image_decode_file(std::string(job.path.c_str()), job.key.box, rgb888,
                                   &s_decoder_cancel, &notes);
        /* The header the decode saw is the whole file's, so it knows the size
           even when the probe's window stopped short of the frame header. */
        Lock lock;
        if (notes.header.width) {
            job.entry->image_width = (uint16_t)notes.header.width;
            job.entry->image_height = (uint16_t)notes.header.height;
            job.entry->image_format = notes.header.format;
            job.entry->image_baseline = notes.header.hardware;
        }
        /* Per file, not per box: a picture that ran out of memory at one size
           runs out at every other one too. A failure is recorded rather than
           taken out of `has_cover`, so the sizes that did decode stay
           reachable through the entry's id. */
        job.entry->image_failed = !pixels && !s_decoder_cancel;
        job.entry->image_too_large = notes.out_of_memory;
    }
    if (!pixels) {
        if (s_decoder_cancel) return;
        ESP_LOGE(TAG, "no picture for %s at %dx%d (%u source bytes, psram free %u, stack left %u)",
                 job.path.c_str(), job.key.box.width, job.key.box.height,
                 (unsigned)job.entry->file_bytes, psram_free(), stack_left());
        if (!job.image) forget_cover(job.entry);
        return;
    }

    if (job.thumbnail) {
        store_thumbnail(job.key, *pixels);
        return;
    }
    hand_over(job, pixels);
    bool stored = false;
    if (!s_decoder_cancel) {
        stored = image_encode(*pixels, [&job](const uint8_t *data, std::size_t size) {
            return store_picture(job.key, data, size);
        });
    }
    /* A request that joined while the picture was being encoded. */
    if (!stored) hand_over(job, pixels);
}

static bool take_request(Request *out) {
    Lock lock;
    for (int priority = 0; priority < 3; priority++) {
        auto &queue = s_state->queues[priority];
        if (queue.empty()) continue;
        *out = std::move(queue.front());
        queue.pop_front();
        out->waited = priority != (int)MetaPriority::Idle;
        s_reader_cancel = false;
        s_reader_token = out->token;
        s_state->reader_path = out->path;
        s_state->reader_want = out->want;
        s_state->reader_box = out->box;
        s_state->reader_waited = out->waited;
        s_state->reader_prefetch_token = out->waited ? 0 : out->token;
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

        /* The job stays this stage's until it has been handed over, waiting for
           decoder room included. The decoder is the slow half, so that wait is
           where the reader spends most of its time and where a cancel almost
           always lands; handing the job over anyway would put a picture nobody
           wants any more in front of the one somebody just asked for. */
        bool room = false;
        if (decode) {
            while (!s_quit && !s_reader_cancel) {
                if (xSemaphoreTake(s_decode_room, pdMS_TO_TICKS(50)) == pdTRUE) {
                    room = true;
                    break;
                }
            }
        }
        bool handed = false;
        const bool cancelled = s_reader_cancel;
        {
            Lock lock;
            if (room && !cancelled) {
                job.token = s_reader_token;
                job.waited = s_state->reader_waited;
                job.prefetch_token = s_state->reader_prefetch_token;
                s_state->decodes.push_back(std::move(job));
                handed = true;
            }
            s_reader_token = 0;
            s_state->reader_path.clear();
            if (!handed && !cancelled) s_state->completed.push_back(request.path);
        }
        if (handed) xSemaphoreGive(s_decode_wake);
        else if (room) xSemaphoreGive(s_decode_room);
        if (s_quit) break;
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
                s_state->decoder_path = job.path;
                s_state->decoder_key = job.key;
                s_state->decoder_thumbnail = job.thumbnail;
                s_state->decoder_waited = job.waited;
                s_state->decoder_prefetch_token = job.prefetch_token;
                s_state->staged = {};
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
            s_state->decoder_path.clear();
            s_state->decoder_waited = false;
        }
        if (s_quit) break;
        if (cancelled || job.notified) continue;
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
    MetaVector<MetaString> done;
    MetaVector<Observer> observers;
    {
        Lock lock;
        done.swap(s_state->completed);
        observers = s_state->observers;
    }
    for (const MetaString &path : done) {
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
    if (!s_meta_heap.create(kMetaHeapBytes)) ESP_LOGE(TAG, "no memory for the meta heap");
    if (!s_thumbnail_heap.create(kThumbnailHeapBytes)) {
        ESP_LOGE(TAG, "no memory for the thumbnail heap");
    }
    if (!s_picture_heap.create(kPictureHeapBytes)) ESP_LOGE(TAG, "no memory for the picture heap");
    image_codec_init();
    s_state = new (memory) CacheState();
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
    s_state->staged = {};
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

std::shared_ptr<const ImagePixels> media_cache_thumbnail(const std::string &path, ImageSize box) {
    if (!s_lock || !box.valid()) return nullptr;
    Lock lock;
    auto entry = find_meta(path);
    if (!entry || !entry->has_cover) return nullptr;
    return take_thumbnail({ entry->image_id, box });
}

/* Called while a reader waits for the JPEG engine: a prefetch nobody is
   waiting for gives way once the frame it is on is done. */
static void yield_prefetch(void *ctx) {
    const ImageKey &key = *static_cast<const ImageKey *>(ctx);
    Lock lock;
    if (s_decoder_token && !s_state->decoder_waited && !(s_state->decoder_key == key)) {
        s_decoder_cancel = true;
    }
}

bool media_cache_read_image(const std::string &path, ImageSize box, uint8_t *dst,
                            std::size_t capacity, ImageSize *size) {
    if (!s_lock || !box.valid() || !dst) return false;
    const bool rgb888 = panel_rgb888();
    const std::size_t pixel_bytes = rgb888 ? 3 : 2;
    if ((uintptr_t)dst % kImageAlignment ||
        capacity < (std::size_t)box.width * box.height * pixel_bytes) {
        ESP_LOGE(TAG, "read_image: buffer does not fit %dx%d", box.width, box.height);
        return false;
    }

    ImageKey key;
    std::shared_ptr<ImagePixels> staged;
    std::shared_ptr<PictureBytes> jpeg;
    {
        Lock lock;
        auto entry = find_meta(path);
        if (!entry || !entry->has_cover) return false;
        key = { entry->image_id, box };
        if (s_state->staged.pixels && s_state->staged.key == key &&
            s_state->staged.pixels->rgb888 == rgb888) {
            staged = std::move(s_state->staged.pixels);
        } else if (auto it = s_state->pictures.find(key); it != s_state->pictures.end()) {
            it->second.used = ++s_state->clock;
            jpeg = it->second.jpeg;
        }
    }

    if (staged) {
        const std::size_t row = (std::size_t)staged->width * pixel_bytes;
        for (uint16_t y = 0; y < staged->height; y++) {
            memcpy(dst + (std::size_t)y * row, staged->data + (std::size_t)y * staged->stride, row);
        }
        *size = { (int16_t)staged->width, (int16_t)staged->height };
        return true;
    }
    if (!jpeg) return false;
    if (image_decode_into(jpeg->data, jpeg->size, box, rgb888, dst, capacity, size,
                          yield_prefetch, &key)) {
        return true;
    }
    /* A re-encoded copy that will not decode is worse than none: drop it so the
       next request goes back to the original picture. */
    Lock lock;
    auto it = s_state->pictures.find(key);
    if (it != s_state->pictures.end() && it->second.jpeg == jpeg) s_state->pictures.erase(it);
    return false;
}

bool media_cache_reserve_pictures() {
    if (!s_lock) return false;
    Lock lock;
    s_release_pictures = false;
    return s_picture_heap.ready() || s_picture_heap.create(kPictureHeapBytes);
}

void media_cache_release_pictures() {
    if (!s_lock) return;
    Lock lock;
    s_state->pictures.clear();
    if (s_live_pictures == 0) s_picture_heap.destroy();
    else s_release_pictures = true;
}

/* A request for the picture a stage is already on joins that job instead of
   repeating it; one that is waited for takes the job over, so withdrawing the
   request that started it no longer stops it. */
static bool join_running(const std::string &path, uint8_t want, ImageSize box, uint32_t token,
                         bool waited) {
    const bool thumbnail = want & MetaWantThumbnail;
    auto same = [&](const MetaString &other) { return strcmp(other.c_str(), path.c_str()) == 0; };
    if (s_decoder_token && s_state->decoder_key.box == box &&
        s_state->decoder_thumbnail == thumbnail && same(s_state->decoder_path)) {
        if (waited) {
            s_decoder_token = token;
            s_state->decoder_waited = true;
        }
        return true;
    }
    for (DecodeJob &job : s_state->decodes) {
        if (!(job.key.box == box) || job.thumbnail != thumbnail || !same(job.path)) continue;
        if (waited) {
            job.token = token;
            job.waited = true;
        }
        return true;
    }
    if (s_reader_token && s_state->reader_box == box && s_state->reader_want == want &&
        same(s_state->reader_path)) {
        if (waited) {
            s_reader_token = token;
            s_state->reader_waited = true;
        }
        return true;
    }
    return false;
}

void media_cache_request(const std::string &path, uint8_t want, ImageSize box,
                         MetaPriority priority, uint32_t token) {
    if (!s_running) return;
    {
        Lock lock;
        const bool waited = priority != MetaPriority::Idle;
        if ((want & (MetaWantThumbnail | MetaWantImage)) && box.valid() &&
            join_running(path, want, box, token, waited)) {
            return;
        }
        MetaDeque<Request> &queue = s_state->queues[(int)priority];
        for (const Request &pending : queue) {
            if (pending.want == want && pending.box == box &&
                strcmp(pending.path.c_str(), path.c_str()) == 0) {
                return;
            }
        }
        queue.push_back({ MetaString(path.c_str()), want, box, token });
    }
    xSemaphoreGive(s_wake);
}

std::shared_ptr<const MediaEntry> media_cache_resolve(const std::string &path, uint8_t want,
                                                      ImageSize box, uint32_t timeout_ms) {
    if (!s_lock) return nullptr;
    auto satisfied = [&]() -> std::shared_ptr<const MediaEntry> {
        Lock lock;
        auto entry = find_meta(path);
        if (!entry) return nullptr;
        if (!(want & (MetaWantThumbnail | MetaWantImage)) || !entry->has_cover || !box.valid()) {
            return entry;
        }
        return cached({ entry->image_id, box }, want & MetaWantThumbnail) ? entry : nullptr;
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

using KeepFn = bool (*)(const char *path, void *ctx);

static void withdraw(uint32_t token, KeepFn keep, void *ctx) {
    if (!s_lock || !token) return;
    auto kept = [&](const MetaString &path) { return keep && keep(path.c_str(), ctx); };
    uint32_t dropped = 0;
    {
        Lock lock;
        for (auto &queue : s_state->queues) {
            for (auto it = queue.begin(); it != queue.end();) {
                it = it->token == token && !kept(it->path) ? queue.erase(it) : it + 1;
            }
        }
        for (auto it = s_state->decodes.begin(); it != s_state->decodes.end();) {
            if (it->token != token || kept(it->path)) {
                ++it;
                continue;
            }
            if (it->prefetch_token) {
                it->token = it->prefetch_token;
                it->waited = false;
                ++it;
                continue;
            }
            it = s_state->decodes.erase(it);
            dropped++;
        }
        if (s_reader_token == token && !kept(s_state->reader_path)) {
            if (s_state->reader_prefetch_token) {
                s_reader_token = s_state->reader_prefetch_token;
                s_state->reader_waited = false;
            } else {
                s_reader_cancel = true;
            }
        }
        if (s_decoder_token == token && !kept(s_state->decoder_path)) {
            if (s_state->decoder_prefetch_token) {
                s_decoder_token = s_state->decoder_prefetch_token;
                s_state->decoder_waited = false;
            } else {
                s_decoder_cancel = true;
            }
        }
    }
    while (dropped--) xSemaphoreGive(s_decode_room);
}

void media_cache_cancel(uint32_t token) {
    withdraw(token, nullptr, nullptr);
}

void media_cache_retain(uint32_t token, KeepFn keep, void *ctx) {
    if (!keep) return;
    withdraw(token, keep, ctx);
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
    if (argc >= 2 && strcmp(argv[1], "drop") == 0) {
        Lock lock;
        s_state->thumbnails.clear();
        s_state->pictures.clear();
        s_state->staged = {};
        harness_reply("OK meta drop");
        return true;
    }
    if (argc >= 2 && strcmp(argv[1], "stats") == 0) {
        Lock lock;
        const auto used = [](const CacheHeap &heap) {
            return (unsigned)(heap.size() - heap.free_bytes());
        };
        ESP_LOGI(TAG,
                 "meta %u images %u thumbnails %u pictures %u psram %u/%u",
                 (unsigned)s_state->meta.size(), (unsigned)s_state->uses.size(),
                 (unsigned)s_state->thumbnails.size(), (unsigned)s_state->pictures.size(),
                 psram_free(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
        ESP_LOGI(TAG, "heaps used: meta %u/%u thumbnail %u/%u picture %u/%u",
                 used(s_meta_heap), (unsigned)s_meta_heap.size(), used(s_thumbnail_heap),
                 (unsigned)s_thumbnail_heap.size(), used(s_picture_heap),
                 (unsigned)s_picture_heap.size());
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
