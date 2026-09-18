/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "player.hpp"
#include "audio/audio_out.hpp"
#include "media/demuxer.hpp"
#include "video/video_presenter.hpp"
#include "h264_dec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include <algorithm>
#include <atomic>
#include <memory>

static const char *TAG = "player";

static constexpr int kVideoSlots = 64;
static constexpr int kMjpegVideoSlots = 4;
static constexpr int kAudioSlots = 8;
static constexpr uint32_t kIdleTimeoutMs = 1500;
static constexpr int64_t kAudioResyncUs = 250000;
static constexpr uint32_t kAudioStackBytes = 20 * 1024;
static constexpr int64_t kKeyframeSkipUs = 500000;
static constexpr int kKeyframeSkipIntervals = 5;
static constexpr int kBacklogSkipIntervals = 3;
static constexpr int64_t kDecodeLeadUs = 120000;
static constexpr int64_t kKeyframeCheckUs = 100000;
static constexpr int64_t kKeyframeJumpUs = 200000;
static constexpr int kMaxReorderFrames = 16;

enum class Command {
    Open,
    Close,
    Play,
    Pause,
    Restart,
    Seek,
    Loop,
    Eject,
};

struct CommandItem {
    Command command;
    std::string *path;
    int64_t value;
};

struct VideoSlot {
    const uint8_t *data;
    std::size_t len;
    int64_t pts_us;
    uint32_t ref;
    int refs;
    bool keyframe;
    bool droppable;
};

struct AudioSlot {
    const uint8_t *data;
    std::size_t len;
    uint32_t ref;
};

static SemaphoreHandle_t s_lock;
static QueueHandle_t s_commands;
static QueueHandle_t s_video_free;
static QueueHandle_t s_video_ready;
static QueueHandle_t s_audio_free;
static QueueHandle_t s_audio_ready;
static SemaphoreHandle_t s_reader_wake;
static SemaphoreHandle_t s_reader_idle;
static SemaphoreHandle_t s_audio_wake;
static SemaphoreHandle_t s_audio_idle;

static VideoSlot s_video[kVideoSlots];
static AudioSlot s_audio[kAudioSlots];
static std::unique_ptr<Demuxer> s_demuxer;
static std::string s_path;
static media_arena_t s_arena;

static bool s_reader_active;
static bool s_audio_active;
static bool s_have_audio;
static bool s_loop;
static std::atomic<bool> s_reader_eof{false};

static PlayerState s_state = PlayerState::Idle;
static std::string s_error;
static std::string s_audio_note;
static CodecId s_audio_codec = CodecId::None;
static CodecId s_video_codec = CodecId::None;
static uint8_t s_nal_length_size;
static bool s_skip_to_keyframe;
static int64_t s_skip_until_us;
static bool s_keyframe_index;
static int64_t s_keyframe_checked_us;
static bool s_seekable;
static int64_t s_shown_us;
static int64_t s_next_us;
static int64_t s_duration_us;
static int64_t s_interval_us;
static int64_t s_reorder_lead_us;
static int64_t s_max_pts_us;

static bool s_want_poster;
static bool s_have_pending;
static int s_pending_slot;
static int64_t s_origin_us;
static int64_t s_origin_pts_us;
static uint64_t s_audio_origin_us;

static void set_state(PlayerState state, const std::string &error = {}) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_state = state;
    s_error = error;
    xSemaphoreGive(s_lock);
}

static void slot_acquire(int slot) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_video[slot].refs++;
    xSemaphoreGive(s_lock);
}

static void slot_release(int slot) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    const bool last = --s_video[slot].refs <= 0;
    if (last) s_video[slot].refs = 0;
    xSemaphoreGive(s_lock);
    if (!last) return;
    if (s_demuxer) s_demuxer->release(s_video[slot].ref);
    xQueueSend(s_video_free, &slot, 0);
}

static void presented(void *ctx) {
    slot_release((int)(intptr_t)ctx);
}

static void refill_free_queues() {
    int slot = -1;
    while (xQueueReceive(s_video_free, &slot, 0) == pdTRUE) {
    }
    while (xQueueReceive(s_video_ready, &slot, 0) == pdTRUE) {
    }
    while (xQueueReceive(s_audio_free, &slot, 0) == pdTRUE) {
    }
    while (xQueueReceive(s_audio_ready, &slot, 0) == pdTRUE) {
    }
    if (s_demuxer) s_demuxer->releaseAll();
    const int video_slots = s_video_codec == CodecId::Mjpeg ? kMjpegVideoSlots : kVideoSlots;
    for (int i = 0; i < kVideoSlots; i++) {
        s_video[i] = {};
        if (i < video_slots) xQueueSend(s_video_free, &i, 0);
    }
    for (int i = 0; i < kAudioSlots; i++) {
        s_audio[i] = {};
        xQueueSend(s_audio_free, &i, 0);
    }
}

static bool take_slot(QueueHandle_t queue, int *slot) {
    while (s_reader_active) {
        if (xQueueReceive(queue, slot, pdMS_TO_TICKS(50)) == pdTRUE) return true;
    }
    return false;
}

static void reader_task(void *) {
    for (;;) {
        xSemaphoreTake(s_reader_wake, portMAX_DELAY);

        bool produced = false;
        while (s_reader_active) {
            Packet packet = {};
            if (!s_demuxer || !s_demuxer->read(s_have_audio, &packet)) {
                if (!s_reader_active) break;
                if (s_demuxer && !s_demuxer->error().empty()) {
                    set_state(PlayerState::Failed, s_demuxer->error());
                    break;
                }
                if (!s_loop || !produced || !s_demuxer->seek(0, nullptr)) {
                    s_reader_eof = true;
                    break;
                }
                produced = false;
                continue;
            }

            const bool video = packet.track == TrackType::Video;
            int slot = -1;
            if (!take_slot(video ? s_video_free : s_audio_free, &slot)) {
                s_demuxer->release(packet.ref);
                break;
            }
            if (video) {
                const bool droppable = s_video_codec == CodecId::H264 &&
                    h264_dec_droppable(packet.data, packet.len, s_nal_length_size);
                s_video[slot] = { packet.data, packet.len, packet.pts_us, packet.ref, 0,
                                  packet.keyframe, droppable };
                produced = true;
                xQueueSend(s_video_ready, &slot, portMAX_DELAY);
            } else {
                s_audio[slot] = { packet.data, packet.len, packet.ref };
                xQueueSend(s_audio_ready, &slot, portMAX_DELAY);
            }
        }

        xSemaphoreGive(s_reader_idle);
    }
}

static void audio_task(void *) {
    for (;;) {
        xSemaphoreTake(s_audio_wake, portMAX_DELAY);

        while (s_audio_active) {
            int slot = -1;
            if (xQueueReceive(s_audio_ready, &slot, pdMS_TO_TICKS(20)) != pdTRUE) continue;
            if (s_audio_active) audio_out_write(s_audio[slot].data, s_audio[slot].len);
            s_demuxer->release(s_audio[slot].ref);
            xQueueSend(s_audio_free, &slot, 0);
        }

        xSemaphoreGive(s_audio_idle);
    }
}

static void audio_start() {
    if (!s_have_audio || s_audio_active) return;
    xSemaphoreTake(s_audio_idle, 0);
    s_audio_active = true;
    xSemaphoreGive(s_audio_wake);
}

static void audio_stop() {
    if (!s_audio_active) return;
    s_audio_active = false;
    if (xSemaphoreTake(s_audio_idle, pdMS_TO_TICKS(kIdleTimeoutMs)) == pdTRUE) {
        xSemaphoreGive(s_audio_idle);
    } else {
        ESP_LOGW(TAG, "audio did not settle");
    }
}

static void reader_stop() {
    audio_stop();
    s_reader_active = false;
    if (s_demuxer) s_demuxer->interrupt(true);
    if (xSemaphoreTake(s_reader_idle, pdMS_TO_TICKS(kIdleTimeoutMs)) == pdTRUE) {
        xSemaphoreGive(s_reader_idle);
    } else {
        ESP_LOGW(TAG, "reader did not settle");
    }

    video_presenter_flush();

    if (s_have_pending) {
        slot_release(s_pending_slot);
        s_have_pending = false;
    }
    refill_free_queues();
}

static void reader_start() {
    xSemaphoreTake(s_reader_idle, 0);
    if (s_demuxer) s_demuxer->interrupt(false);
    s_reader_eof = false;
    s_reader_active = true;
    xSemaphoreGive(s_reader_wake);
}

static int64_t media_clock_us() {
    const int64_t now = esp_timer_get_time();
    int64_t elapsed = now - s_origin_us;
    const bool video_backlog = uxQueueMessagesWaiting(s_video_free) == 0;
    if (s_have_audio && audio_out_running() && !video_backlog) {
        const int64_t audio = (int64_t)audio_out_position_us() - (int64_t)s_audio_origin_us;
        if (audio < elapsed - kAudioResyncUs) {
            s_origin_us = now - audio;
            elapsed = audio;
        }
    }
    return elapsed;
}

static bool before(int64_t pts_us, int64_t mark_us) {
    return pts_us + s_interval_us / 2 < mark_us;
}

static void submit(int slot, bool present, int64_t due_us) {
    slot_acquire(slot);
    if (!video_presenter_submit(s_video[slot].data, s_video[slot].len, presented,
                                (void *)(intptr_t)slot, present, due_us)) {
        slot_release(slot);
    }
}

static void show(int slot, int64_t due_us = 0) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_video[slot].pts_us > s_shown_us || !s_reorder_lead_us) s_shown_us = s_video[slot].pts_us;
    s_next_us = s_video[slot].pts_us + s_interval_us;
    xSemaphoreGive(s_lock);
    submit(slot, true, due_us);
    s_want_poster = false;
}

static bool interframe_codec() {
    return s_video_codec == CodecId::H264;
}

static void skip_to_keyframe(int64_t until_us) {
    s_skip_to_keyframe = true;
    s_skip_until_us = until_us;
}

static void cancel_skip() {
    s_skip_to_keyframe = false;
    s_skip_until_us = INT64_MIN;
}

static bool due_keyframe_after(int64_t pts, int64_t now, int64_t *key_us) {
    const int64_t wall = esp_timer_get_time();
    if (wall - s_keyframe_checked_us < kKeyframeCheckUs) return false;
    s_keyframe_checked_us = wall;
    return s_demuxer->keyframeBefore(now + s_origin_pts_us, key_us) && before(pts, *key_us);
}

static void close_source() {
    audio_out_close();
    if (s_demuxer) {
        s_demuxer->close();
        s_demuxer.reset();
    }
    s_have_audio = false;
}

static void reset_timeline() {
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_audio_note.clear();
    s_audio_codec = CodecId::None;
    s_video_codec = CodecId::None;
    s_seekable = false;
    s_duration_us = 0;
    s_interval_us = 0;
    s_shown_us = 0;
    s_next_us = 0;
    s_reorder_lead_us = 0;
    s_max_pts_us = INT64_MIN;
    xSemaphoreGive(s_lock);
}

static void fail_open(const std::string &error) {
    close_source();
    set_state(PlayerState::Failed, error);
}

static void handle_open(const std::string &path) {
    reader_stop();
    close_source();
    s_loop = false;
    s_want_poster = false;
    reset_timeline();
    set_state(PlayerState::Loading);

    s_path = path;
    s_demuxer = demuxer_create(path);
    if (!s_demuxer) {
        set_state(PlayerState::Failed, "unsupported file");
        return;
    }
    if (!s_demuxer->open(path, s_arena)) {
        fail_open(s_demuxer->error());
        return;
    }

    const MediaInfo &info = s_demuxer->info();
    if (info.video.codec != CodecId::Mjpeg && info.video.codec != CodecId::H264) {
        fail_open("unsupported video codec");
        return;
    }
    if (info.frame_interval_us <= 0 || info.duration_us <= 0) {
        fail_open("video has no timeline");
        return;
    }
    std::string video_error;
    if (!video_presenter_open_stream(info.video, &video_error)) {
        fail_open(video_error);
        return;
    }
    s_nal_length_size = info.video.nal_length_size;
    cancel_skip();
    int64_t key_us = 0;
    s_keyframe_index = s_demuxer->keyframeBefore(info.duration_us, &key_us);

    video_presenter_set_source_rotation(info.video.rotation);

    std::string note;
    s_have_audio = audio_out_open(info.audio, info.video.codec != CodecId::H264, &note);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_duration_us = info.duration_us;
    s_interval_us = info.frame_interval_us;
    s_seekable = info.seekable;
    s_audio_codec = info.audio.codec;
    s_video_codec = info.video.codec;
    s_audio_note = note;
    xSemaphoreGive(s_lock);

    refill_free_queues();
    s_want_poster = true;
    set_state(PlayerState::Paused);
    reader_start();
}

static void handle_close() {
    reader_stop();
    close_source();
    reset_timeline();
    s_want_poster = false;
    set_state(PlayerState::Idle);
}

static void handle_eject(const std::string &mount_point) {
    if (!s_demuxer || s_path.compare(0, mount_point.size(), mount_point) != 0) return;
    if (s_path.size() > mount_point.size() && s_path[mount_point.size()] != '/') return;
    reader_stop();
    close_source();
    s_want_poster = false;
    set_state(PlayerState::Failed, "storage removed");
}

static bool rewind_to(int64_t position_us) {
    if (!s_demuxer || !s_demuxer->isOpen()) return false;
    reader_stop();
    int64_t landed_us = position_us;
    if (!s_demuxer->seek(position_us, &landed_us)) {
        reader_start();
        return false;
    }
    audio_out_flush();
    cancel_skip();
    s_max_pts_us = INT64_MIN;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_shown_us = landed_us;
    s_next_us = landed_us;
    xSemaphoreGive(s_lock);
    s_want_poster = true;
    reader_start();
    return true;
}

static void handle_play() {
    if (!s_demuxer || !s_demuxer->isOpen()) return;
    if (s_state == PlayerState::Finished || s_next_us >= s_duration_us) {
        if (!rewind_to(0)) return;
    }
    s_origin_us = esp_timer_get_time();
    s_origin_pts_us = s_next_us;
    s_audio_origin_us = audio_out_position_us();
    set_state(PlayerState::Playing);
    audio_start();
}

static void handle_pause() {
    if (s_state != PlayerState::Playing) return;
    audio_stop();
    set_state(PlayerState::Paused);
}

static void handle_restart() {
    if (!rewind_to(0)) return;
    set_state(PlayerState::Paused);
}

static void handle_seek(int64_t position_us) {
    if (!s_demuxer || !s_demuxer->isOpen() || s_interval_us <= 0) return;
    const bool playing = s_state == PlayerState::Playing;
    if (position_us >= s_duration_us) position_us = s_duration_us - s_interval_us;
    if (position_us < 0) position_us = 0;
    position_us = position_us / s_interval_us * s_interval_us;
    if (!rewind_to(position_us)) return;
    if (playing) {
        handle_play();
    } else {
        set_state(PlayerState::Paused);
    }
}

static void handle_loop(bool loop) {
    s_loop = loop;
    if (!loop || !s_demuxer || !s_demuxer->isOpen()) return;
    if (s_state != PlayerState::Playing && s_state != PlayerState::Paused) return;
    if (xSemaphoreTake(s_reader_idle, 0) != pdTRUE) return;
    if (s_reader_active && s_demuxer->seek(0, nullptr)) {
        s_reader_eof = false;
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
    }
    delete item.path;
}

static void step_poster() {
    int slot = -1;
    if (xQueueReceive(s_video_ready, &slot, pdMS_TO_TICKS(20)) != pdTRUE) return;
    slot_acquire(slot);
    if (!before(s_video[slot].pts_us, s_next_us)) {
        show(slot);
        video_presenter_drain();
    }
    slot_release(slot);
}

static void step_playing() {
    if (!s_have_pending) {
        const bool drained = s_reader_eof;
        if (xQueueReceive(s_video_ready, &s_pending_slot, pdMS_TO_TICKS(20)) != pdTRUE) {
            if (!s_loop && drained) {
                video_presenter_drain();
                audio_stop();
                set_state(PlayerState::Finished);
            }
            return;
        }
        slot_acquire(s_pending_slot);
        s_have_pending = true;
    }

    const int64_t pts = s_video[s_pending_slot].pts_us;
    if (pts > s_max_pts_us) {
        s_max_pts_us = pts;
    } else if (s_max_pts_us - pts > s_reorder_lead_us) {
        s_reorder_lead_us = std::min(s_max_pts_us - pts, s_interval_us * kMaxReorderFrames);
    }
    if (before(pts, s_origin_pts_us)) {
        if (!s_loop) {
            s_have_pending = false;
            if (interframe_codec()) submit(s_pending_slot, false, 0);
            slot_release(s_pending_slot);
            return;
        }
        s_origin_us += s_next_us - s_origin_pts_us;
        s_origin_pts_us = pts;
        s_skip_until_us = INT64_MIN;
        s_audio_origin_us = (uint64_t)((int64_t)audio_out_position_us() -
                                       (esp_timer_get_time() - s_origin_us));
    }
    const int64_t due = pts - s_origin_pts_us;
    const int64_t now = media_clock_us();
    const int slot = s_pending_slot;
    const VideoSlot &frame = s_video[slot];
    if (s_skip_to_keyframe && (!frame.keyframe || before(pts, s_skip_until_us))) {
        s_have_pending = false;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_next_us = pts + s_interval_us;
        xSemaphoreGive(s_lock);
        slot_release(slot);
        return;
    }
    const int64_t lead = video_presenter_pipelined() ? kDecodeLeadUs : 0;
    const int64_t decode_due = due - s_reorder_lead_us;
    if (now < decode_due - lead) {
        const TickType_t ticks = pdMS_TO_TICKS((decode_due - lead - now) / 1000);
        vTaskDelay(ticks ? ticks : 1);
        return;
    }
    const int64_t due_at = lead ? esp_timer_get_time() + (due - now) : 0;

    s_have_pending = false;
    const bool last = pts + s_interval_us >= s_duration_us;
    const bool resuming = s_skip_to_keyframe;
    cancel_skip();
    if (!last && now > due + s_interval_us) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_next_us = pts + s_interval_us;
        xSemaphoreGive(s_lock);
        if (interframe_codec() && !frame.droppable) {
            const int64_t late = now - due;
            const bool backlog = uxQueueMessagesWaiting(s_video_free) == 0;
            const int64_t limit = backlog ? s_interval_us * kBacklogSkipIntervals
                                          : std::max(kKeyframeSkipUs, s_interval_us * kKeyframeSkipIntervals);
            int64_t key_us = 0;
            if (!resuming && s_keyframe_index && late > kKeyframeJumpUs &&
                due_keyframe_after(pts, now, &key_us)) {
                skip_to_keyframe(key_us);
            } else if (!resuming && !frame.keyframe && (backlog || !s_keyframe_index) &&
                       late > limit) {
                skip_to_keyframe(INT64_MIN);
            } else {
                show(slot);
            }
        }
        slot_release(slot);
        return;
    }

    show(slot, due_at);
    slot_release(slot);
}

static void player_task(void *) {
    for (;;) {
        const bool busy = s_state == PlayerState::Playing ||
                          (s_state == PlayerState::Paused && s_want_poster);
        CommandItem item = {};
        if (xQueueReceive(s_commands, &item, busy ? 0 : portMAX_DELAY) == pdTRUE) {
            handle_command(item);
            continue;
        }
        if (s_state == PlayerState::Playing) {
            step_playing();
        } else if (s_state == PlayerState::Paused && s_want_poster) {
            step_poster();
        }
    }
}

static void send_command(Command command, const std::string *path = nullptr, int64_t value = 0) {
    if (!s_commands) return;
    CommandItem item = { command, path ? new std::string(*path) : nullptr, value };
    if (xQueueSend(s_commands, &item, pdMS_TO_TICKS(100)) != pdTRUE) delete item.path;
}

void player_start(const media_arena_t &arena) {
    if (s_lock) return;
    s_arena = arena;
    s_lock = xSemaphoreCreateMutex();
    s_commands = xQueueCreate(4, sizeof(CommandItem));
    s_video_free = xQueueCreate(kVideoSlots, sizeof(int));
    s_video_ready = xQueueCreate(kVideoSlots, sizeof(int));
    s_audio_free = xQueueCreate(kAudioSlots, sizeof(int));
    s_audio_ready = xQueueCreate(kAudioSlots, sizeof(int));
    s_reader_wake = xSemaphoreCreateBinary();
    s_reader_idle = xSemaphoreCreateBinary();
    s_audio_wake = xSemaphoreCreateBinary();
    s_audio_idle = xSemaphoreCreateBinary();
    xSemaphoreGive(s_reader_idle);
    xSemaphoreGive(s_audio_idle);
    audio_out_start();
    xTaskCreate(reader_task, "media_reader", 4096, nullptr, 4, nullptr);
    xTaskCreate(audio_task, "media_audio", kAudioStackBytes, nullptr, 6, nullptr);
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

PlayerStatus player_status() {
    PlayerStatus status = {};
    if (!s_lock) return status;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    status.state = s_state;
    status.duration_us = s_duration_us;
    status.position_us = s_state == PlayerState::Finished ? s_duration_us : s_shown_us;
    status.seekable = s_seekable;
    status.loop = s_loop;
    status.audio_codec = s_audio_codec;
    status.video_codec = s_video_codec;
    status.audio_note = s_audio_note;
    status.error = s_error;
    xSemaphoreGive(s_lock);
    return status;
}
