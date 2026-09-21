/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "player_internal.hpp"
#include "audio/audio_out.hpp"
#include "media/demuxer.hpp"
#include "h264_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <memory>

static const char *TAG = "player";

static constexpr int kAudioSlots = 8;
static constexpr uint32_t kIdleTimeoutMs = 1500;
static constexpr int64_t kAudioResyncUs = 250000;
static constexpr uint32_t kAudioStackBytes = 4 * 1024;
static constexpr uint32_t kOpusStackBytes = 20 * 1024;

enum class Command {
    Open,
    Close,
    Play,
    Pause,
    Restart,
    Seek,
    Loop,
    Eject,
    Repaint,
};

struct CommandItem {
    Command command;
    std::string *path;
    int64_t value;
};

struct AudioSlot {
    const uint8_t *data;
    std::size_t len;
    uint32_t ref;
};

PlayerCore player_core;

static QueueHandle_t s_commands;
static QueueHandle_t s_audio_free;
static QueueHandle_t s_audio_ready;
static SemaphoreHandle_t s_reader_wake;
static SemaphoreHandle_t s_reader_idle;
static SemaphoreHandle_t s_audio_wake;
static SemaphoreHandle_t s_audio_idle;
static TaskHandle_t s_audio_task;
static bool s_audio_quit;

static AudioSlot s_audio[kAudioSlots];
static std::string s_path;
static media_arena_t s_arena;

static bool s_audio_active;
static bool s_have_audio;

static MediaSummary s_summary;
static std::string s_audio_note;
static CodecId s_audio_codec = CodecId::None;
static bool s_seekable;

void player_set_state(PlayerState state, const std::string &error) {
    xSemaphoreTake(player_core.lock, portMAX_DELAY);
    player_core.state = state;
    player_core.error = error;
    xSemaphoreGive(player_core.lock);
}

bool player_take_slot(QueueHandle_t queue, int *slot) {
    while (player_core.reader_active) {
        if (xQueueReceive(queue, slot, pdMS_TO_TICKS(50)) == pdTRUE) return true;
    }
    return false;
}

static void refill_free_queues() {
    int slot = -1;
    video_pacing_drain_queues();
    while (xQueueReceive(s_audio_free, &slot, 0) == pdTRUE) {
    }
    while (xQueueReceive(s_audio_ready, &slot, 0) == pdTRUE) {
    }
    if (player_core.demuxer) player_core.demuxer->releaseAll();
    video_pacing_arm_slots();
    for (int i = 0; i < kAudioSlots; i++) {
        s_audio[i] = {};
        xQueueSend(s_audio_free, &i, 0);
    }
}

static void reader_task(void *) {
    for (;;) {
        xSemaphoreTake(s_reader_wake, portMAX_DELAY);

        bool produced = false;
        while (player_core.reader_active) {
            Packet packet = {};
            if (!player_core.demuxer || !player_core.demuxer->read(s_have_audio, &packet)) {
                if (!player_core.reader_active) break;
                if (player_core.demuxer && !player_core.demuxer->error().empty()) {
                    player_set_state(PlayerState::Failed, player_core.demuxer->error());
                    break;
                }
                if (!player_core.loop || !produced || !player_core.demuxer->seek(0, nullptr)) {
                    player_core.reader_eof = true;
                    break;
                }
                produced = false;
                continue;
            }

            if (packet.track == TrackType::Video) {
                if (!video_pacing_enqueue(packet)) {
                    player_core.demuxer->release(packet.ref);
                    break;
                }
                produced = true;
            } else {
                int slot = -1;
                if (!player_take_slot(s_audio_free, &slot)) {
                    player_core.demuxer->release(packet.ref);
                    break;
                }
                s_audio[slot] = { packet.data, packet.len, packet.ref };
                xQueueSend(s_audio_ready, &slot, portMAX_DELAY);
            }
        }

        xSemaphoreGive(s_reader_idle);
    }
}

static void audio_task(void *) {
    while (!s_audio_quit) {
        xSemaphoreTake(s_audio_wake, portMAX_DELAY);

        while (s_audio_active) {
            int slot = -1;
            if (xQueueReceive(s_audio_ready, &slot, pdMS_TO_TICKS(20)) != pdTRUE) continue;
            if (s_audio_active) audio_out_write(s_audio[slot].data, s_audio[slot].len);
            player_core.demuxer->release(s_audio[slot].ref);
            xQueueSend(s_audio_free, &slot, 0);
        }

        xSemaphoreGive(s_audio_idle);
    }
#ifdef ESP_PLATFORM
    vTaskDeleteWithCaps(nullptr);
#else
    vTaskDelete(nullptr);
#endif
}

#ifdef ESP_PLATFORM
static BaseType_t audio_task_create(uint32_t stack_bytes, uint32_t caps) {
    return xTaskCreatePinnedToCoreWithCaps(audio_task, "media_audio", stack_bytes, nullptr, 6,
                                           &s_audio_task, tskNO_AFFINITY, caps | MALLOC_CAP_8BIT);
}
#endif

static bool audio_task_start(uint32_t stack_bytes) {
    s_audio_quit = false;
#ifdef ESP_PLATFORM
    BaseType_t created = pdFAIL;
    if (stack_bytes <= kAudioStackBytes) {
        created = audio_task_create(stack_bytes, MALLOC_CAP_INTERNAL);
        if (created != pdPASS) ESP_LOGW(TAG, "audio task stack falls back to PSRAM");
    }
    if (created != pdPASS) created = audio_task_create(stack_bytes, MALLOC_CAP_SPIRAM);
#else
    const BaseType_t created =
        xTaskCreate(audio_task, "media_audio", stack_bytes, nullptr, 6, &s_audio_task);
#endif
    if (created != pdPASS) {
        s_audio_task = nullptr;
        return false;
    }
    return true;
}

static void audio_task_stop() {
    if (!s_audio_task) return;
    s_audio_quit = true;
    xSemaphoreTake(s_audio_idle, 0);
    xSemaphoreGive(s_audio_wake);
    if (xSemaphoreTake(s_audio_idle, pdMS_TO_TICKS(kIdleTimeoutMs)) != pdTRUE) {
        ESP_LOGW(TAG, "audio task did not exit");
    }
    xSemaphoreGive(s_audio_idle);
    s_audio_task = nullptr;
}

static void audio_start() {
    if (!s_have_audio || s_audio_active) return;
    xSemaphoreTake(s_audio_idle, 0);
    s_audio_active = true;
    xSemaphoreGive(s_audio_wake);
}

void player_audio_stop() {
    if (!s_audio_active) return;
    s_audio_active = false;
    if (xSemaphoreTake(s_audio_idle, pdMS_TO_TICKS(kIdleTimeoutMs)) == pdTRUE) {
        xSemaphoreGive(s_audio_idle);
    } else {
        ESP_LOGW(TAG, "audio did not settle");
    }
}

static void reader_stop() {
    player_audio_stop();
    player_core.reader_active = false;
    if (player_core.demuxer) player_core.demuxer->interrupt(true);
    if (xSemaphoreTake(s_reader_idle, pdMS_TO_TICKS(kIdleTimeoutMs)) == pdTRUE) {
        xSemaphoreGive(s_reader_idle);
    } else {
        ESP_LOGW(TAG, "reader did not settle");
    }

    video_pacing_stop();
    refill_free_queues();
}

static void reader_start() {
    xSemaphoreTake(s_reader_idle, 0);
    if (player_core.demuxer) player_core.demuxer->interrupt(false);
    player_core.reader_eof = false;
    player_core.reader_active = true;
    xSemaphoreGive(s_reader_wake);
}

int64_t player_media_clock_us() {
    const int64_t now = esp_timer_get_time();
    int64_t elapsed = now - player_core.origin_us;
    if (s_have_audio && audio_out_running() && !video_pacing_backlog()) {
        const int64_t audio =
            (int64_t)audio_out_position_us() - (int64_t)player_core.audio_origin_us;
        if (audio < elapsed - kAudioResyncUs) {
            player_core.origin_us = now - audio;
            elapsed = audio;
        }
    }
    return elapsed;
}

static void close_source() {
    audio_task_stop();
    audio_out_close();
    if (player_core.demuxer) {
        player_core.demuxer->close();
        player_core.demuxer.reset();
    }
    s_have_audio = false;
}

static void reset_timeline() {
    xSemaphoreTake(player_core.lock, portMAX_DELAY);
    s_summary = {};
    s_audio_note.clear();
    s_audio_codec = CodecId::None;
    player_core.video_codec = CodecId::None;
    s_seekable = false;
    player_core.duration_us = 0;
    player_core.interval_us = 0;
    player_core.shown_us = 0;
    player_core.next_us = 0;
    video_pacing_reset_timeline();
    xSemaphoreGive(player_core.lock);
}

static void fail_open(const std::string &error) {
    close_source();
    player_set_state(PlayerState::Failed, error);
}

static void handle_open(const std::string &path) {
    reader_stop();
    close_source();
    player_core.loop = false;
    video_pacing_set_poster(false);
    reset_timeline();
    player_set_state(PlayerState::Loading);

    s_path = path;
    player_core.demuxer = demuxer_create(path);
    if (!player_core.demuxer) {
        player_set_state(PlayerState::Failed, "unsupported file");
        return;
    }
    if (!player_core.demuxer->open(path, s_arena)) {
        fail_open(player_core.demuxer->error());
        return;
    }

    const MediaInfo &info = player_core.demuxer->info();
    if (!video_pacing_codec_supported(info.video.codec)) {
        fail_open("unsupported video codec");
        return;
    }
    if (info.frame_interval_us <= 0 || info.duration_us <= 0) {
        fail_open("video has no timeline");
        return;
    }
    std::string video_error;
    if (!video_pacing_open(info, &video_error)) {
        fail_open(video_error);
        return;
    }

    std::string note;
    s_have_audio = audio_out_open(info.audio, info.video.codec == CodecId::Mjpeg, &note);
    if (s_have_audio &&
        !audio_task_start(info.audio.codec == CodecId::Opus ? kOpusStackBytes : kAudioStackBytes)) {
        audio_out_close();
        s_have_audio = false;
        note = "no memory for the audio task";
    }

    MediaSummary summary;
    summary.valid = true;
    summary.container = demuxer_format_name(path);
    summary.file_bytes = player_core.demuxer->bytes();
    summary.duration_us = info.duration_us;
    summary.seekable = info.seekable;
    summary.video.codec = info.video.codec;
    summary.video.width = info.video.width;
    summary.video.height = info.video.height;
    summary.video.frame_interval_us = info.frame_interval_us;
    summary.video.rotation = info.video.rotation;
    if (info.video.codec == CodecId::H264 && !info.video.codec_private.empty()) {
        h264_dec_stream_info_t stream = {};
        const char *failure = nullptr;
        if (h264_dec_probe(info.video.codec_private.data(), info.video.codec_private.size(), 0,
                           &stream, &failure)) {
            summary.video.profile_idc = stream.profile_idc;
            summary.video.level_idc = stream.level_idc;
        }
    }
    summary.audio.codec = info.audio.codec;
    summary.audio.sample_rate = info.audio.sample_rate;
    summary.audio.bitrate_bps = info.audio.bitrate_bps;
    summary.audio.channels = info.audio.channels;
    summary.audio.bits = info.audio.bits;
    if (!summary.audio.bitrate_bps && info.audio.codec == CodecId::Pcm) {
        summary.audio.bitrate_bps = info.audio.sample_rate * info.audio.channels * info.audio.bits;
    }
    summary.audio.note = note;

    xSemaphoreTake(player_core.lock, portMAX_DELAY);
    s_summary = summary;
    player_core.duration_us = info.duration_us;
    player_core.interval_us = info.frame_interval_us;
    s_seekable = info.seekable;
    s_audio_codec = info.audio.codec;
    player_core.video_codec = info.video.codec;
    s_audio_note = note;
    xSemaphoreGive(player_core.lock);

    refill_free_queues();
    video_pacing_set_poster(true);
    player_set_state(PlayerState::Paused);
    reader_start();
}

static void handle_close() {
    reader_stop();
    close_source();
    reset_timeline();
    video_pacing_set_poster(false);
    player_set_state(PlayerState::Idle);
}

static void handle_eject(const std::string &mount_point) {
    if (!player_core.demuxer || s_path.compare(0, mount_point.size(), mount_point) != 0) return;
    if (s_path.size() > mount_point.size() && s_path[mount_point.size()] != '/') return;
    reader_stop();
    close_source();
    video_pacing_set_poster(false);
    player_set_state(PlayerState::Failed, "storage removed");
}

static bool rewind_to(int64_t position_us) {
    if (!player_core.demuxer || !player_core.demuxer->isOpen()) return false;
    reader_stop();
    int64_t landed_us = position_us;
    if (!player_core.demuxer->seek(position_us, &landed_us)) {
        reader_start();
        return false;
    }
    audio_out_flush();
    video_pacing_rewound();
    xSemaphoreTake(player_core.lock, portMAX_DELAY);
    player_core.shown_us = landed_us;
    player_core.next_us = landed_us;
    xSemaphoreGive(player_core.lock);
    video_pacing_set_poster(true);
    reader_start();
    return true;
}

static void handle_play() {
    if (!player_core.demuxer || !player_core.demuxer->isOpen()) return;
    if (player_core.state == PlayerState::Finished ||
        player_core.next_us >= player_core.duration_us) {
        if (!rewind_to(0)) return;
    }
    player_core.origin_us = esp_timer_get_time();
    player_core.origin_pts_us = player_core.next_us;
    player_core.audio_origin_us = audio_out_position_us();
    player_set_state(PlayerState::Playing);
    audio_start();
}

static void handle_pause() {
    if (player_core.state != PlayerState::Playing) return;
    player_audio_stop();
    player_set_state(PlayerState::Paused);
}

static void handle_restart() {
    if (!rewind_to(0)) return;
    player_set_state(PlayerState::Paused);
}

static void handle_seek(int64_t position_us) {
    if (!player_core.demuxer || !player_core.demuxer->isOpen() || player_core.interval_us <= 0) {
        return;
    }
    const bool playing = player_core.state == PlayerState::Playing;
    if (position_us >= player_core.duration_us) {
        position_us = player_core.duration_us - player_core.interval_us;
    }
    if (position_us < 0) position_us = 0;
    position_us = position_us / player_core.interval_us * player_core.interval_us;
    if (!rewind_to(position_us)) return;
    if (playing) {
        handle_play();
    } else {
        player_set_state(PlayerState::Paused);
    }
}

static void handle_loop(bool loop) {
    player_core.loop = loop;
    if (!loop || !player_core.demuxer || !player_core.demuxer->isOpen()) return;
    if (player_core.state != PlayerState::Playing && player_core.state != PlayerState::Paused) {
        return;
    }
    if (xSemaphoreTake(s_reader_idle, 0) != pdTRUE) return;
    if (player_core.reader_active && player_core.demuxer->seek(0, nullptr)) {
        player_core.reader_eof = false;
        xSemaphoreGive(s_reader_wake);
    } else {
        xSemaphoreGive(s_reader_idle);
    }
}

static void handle_command(const CommandItem &item) {
    switch (item.command) {
    case Command::Open:    handle_open(*item.path); break;
    case Command::Close:   handle_close(); break;
    case Command::Play:    handle_play(); break;
    case Command::Pause:   handle_pause(); break;
    case Command::Restart: handle_restart(); break;
    case Command::Seek:    handle_seek(item.value); break;
    case Command::Loop:    handle_loop(item.value != 0); break;
    case Command::Eject:   handle_eject(*item.path); break;
    case Command::Repaint: video_pacing_repaint(); break;
    }
    delete item.path;
}

static void player_task(void *) {
    for (;;) {
        const bool busy = player_core.state == PlayerState::Playing ||
                          (player_core.state == PlayerState::Paused && video_pacing_wants_poster());
        CommandItem item = {};
        if (xQueueReceive(s_commands, &item, busy ? 0 : portMAX_DELAY) == pdTRUE) {
            handle_command(item);
            continue;
        }
        if (player_core.state == PlayerState::Playing) {
            video_pacing_step();
        } else if (player_core.state == PlayerState::Paused && video_pacing_wants_poster()) {
            video_pacing_step_poster();
        }
    }
}

static void send_command(Command command, const std::string *path = nullptr, int64_t value = 0) {
    if (!s_commands) return;
    CommandItem item = { command, path ? new std::string(*path) : nullptr, value };
    if (xQueueSend(s_commands, &item, pdMS_TO_TICKS(100)) != pdTRUE) delete item.path;
}

void player_start(const media_arena_t &arena) {
    if (player_core.lock) return;
    s_arena = arena;
    player_core.lock = xSemaphoreCreateMutex();
    s_commands = xQueueCreate(4, sizeof(CommandItem));
    s_audio_free = xQueueCreate(kAudioSlots, sizeof(int));
    s_audio_ready = xQueueCreate(kAudioSlots, sizeof(int));
    s_reader_wake = xSemaphoreCreateBinary();
    s_reader_idle = xSemaphoreCreateBinary();
    s_audio_wake = xSemaphoreCreateBinary();
    s_audio_idle = xSemaphoreCreateBinary();
    xSemaphoreGive(s_reader_idle);
    xSemaphoreGive(s_audio_idle);
    video_pacing_start();
    audio_out_start();
    xTaskCreate(reader_task, "media_reader", 4096, nullptr, 4, nullptr);
    xTaskCreate(player_task, "player", 6144, nullptr, 5, nullptr);
}

void player_open(const std::string &path) { send_command(Command::Open, &path); }
void player_close() { send_command(Command::Close); }
void player_play() { send_command(Command::Play); }
void player_pause() { send_command(Command::Pause); }
void player_restart() { send_command(Command::Restart); }
void player_seek(int64_t position_us) { send_command(Command::Seek, nullptr, position_us); }
void player_set_loop(bool loop) { send_command(Command::Loop, nullptr, loop ? 1 : 0); }
void player_eject(const std::string &mount_point) { send_command(Command::Eject, &mount_point); }
void player_repaint() { send_command(Command::Repaint); }

MediaSummary player_media_summary() {
    MediaSummary summary;
    if (!player_core.lock) return summary;

    xSemaphoreTake(player_core.lock, portMAX_DELAY);
    summary = s_summary;
    xSemaphoreGive(player_core.lock);
    return summary;
}

PlayerStatus player_status() {
    PlayerStatus status = {};
    if (!player_core.lock) return status;

    xSemaphoreTake(player_core.lock, portMAX_DELAY);
    status.state = player_core.state;
    status.duration_us = player_core.duration_us;
    status.position_us =
        player_core.state == PlayerState::Finished ? player_core.duration_us : player_core.shown_us;
    status.seekable = s_seekable;
    status.loop = player_core.loop;
    status.audio_codec = s_audio_codec;
    status.video_codec = player_core.video_codec;
    status.audio_note = s_audio_note;
    status.error = player_core.error;
    xSemaphoreGive(player_core.lock);
    return status;
}
