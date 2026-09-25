/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "player_internal.hpp"
#include "video/video_presenter.hpp"
#include "h264_dec.h"
#include "mpeg2_dec.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include <algorithm>

static constexpr int kVideoSlots = 64;
static constexpr int kMjpegVideoSlots = 4;
static constexpr int64_t kKeyframeSkipUs = 500000;
static constexpr int kKeyframeSkipIntervals = 5;
static constexpr int kBacklogSkipIntervals = 3;
static constexpr int64_t kDecodeLeadUs = 120000;
static constexpr int64_t kKeyframeCheckUs = 100000;
static constexpr int64_t kKeyframeJumpUs = 200000;
static constexpr int kMaxReorderFrames = 16;

struct VideoSlot {
    const uint8_t *data;
    std::size_t len;
    int64_t pts_us;
    uint32_t ref;
    int refs;
    bool keyframe;
    bool droppable;
};

static QueueHandle_t s_free;
static QueueHandle_t s_ready;
static VideoSlot s_video[kVideoSlots];

static uint8_t s_nal_length_size;
static bool s_skip_to_keyframe;
static int64_t s_skip_until_us;
static bool s_keyframe_index;
static int64_t s_keyframe_checked_us;
static int64_t s_reorder_lead_us;
static int64_t s_max_pts_us = INT64_MIN;

static bool s_want_poster;
static bool s_seek_frame;
static bool s_have_pending;
static int s_pending_slot;
static int s_repaint_slot = -1;

static void slot_acquire(int slot) {
    xSemaphoreTake(player_core.lock, portMAX_DELAY);
    s_video[slot].refs++;
    xSemaphoreGive(player_core.lock);
}

static void slot_release(int slot) {
    xSemaphoreTake(player_core.lock, portMAX_DELAY);
    const bool last = --s_video[slot].refs <= 0;
    if (last) s_video[slot].refs = 0;
    xSemaphoreGive(player_core.lock);
    if (!last) return;
    if (player_core.demuxer) player_core.demuxer->release(s_video[slot].ref);
    xQueueSend(s_free, &slot, 0);
}

static void presented(void *ctx) {
    slot_release((int)(intptr_t)ctx);
}

static void keep_for_repaint(int slot) {
    if (slot == s_repaint_slot) return;
    const int previous = s_repaint_slot;
    slot_acquire(slot);
    s_repaint_slot = slot;
    if (previous >= 0) slot_release(previous);
}

static void forget_repaint() {
    if (s_repaint_slot < 0) return;
    slot_release(s_repaint_slot);
    s_repaint_slot = -1;
}

static bool before(int64_t pts_us, int64_t mark_us) {
    return pts_us + player_core.interval_us / 2 < mark_us;
}

// An MJPEG seek lands on the frame on screen at the target, which can start
// well before it.
static void take_seek_frame(int slot) {
    if (!s_seek_frame) return;
    s_seek_frame = false;
    s_video[slot].pts_us = std::max(s_video[slot].pts_us, player_core.next_us);
}

static void submit(int slot, bool present, int64_t due_us) {
    slot_acquire(slot);
    if (!video_presenter_submit(s_video[slot].data, s_video[slot].len, presented,
                                (void *)(intptr_t)slot, present, due_us)) {
        slot_release(slot);
    }
}

static void show(int slot, int64_t due_us = 0) {
    xSemaphoreTake(player_core.lock, portMAX_DELAY);
    if (s_video[slot].pts_us > player_core.shown_us || !s_reorder_lead_us) {
        player_core.shown_us = s_video[slot].pts_us;
    }
    player_core.next_us = s_video[slot].pts_us + player_core.interval_us;
    xSemaphoreGive(player_core.lock);
    submit(slot, true, due_us);
    s_want_poster = false;
}

static bool interframe_codec() {
    return player_core.video_codec == CodecId::H264 || player_core.video_codec == CodecId::Mpeg2;
}

static bool droppable_packet(const Packet &packet) {
    switch (player_core.video_codec) {
    case CodecId::H264: return h264_dec_droppable(packet.data, packet.len, s_nal_length_size);
    case CodecId::Mpeg2: return mpeg2_dec_droppable(packet.data, packet.len);
    default: return false;
    }
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
    return player_core.demuxer->keyframeBefore(now + player_core.origin_pts_us, key_us) &&
           before(pts, *key_us);
}

void video_pacing_start() {
    s_free = xQueueCreate(kVideoSlots, sizeof(int));
    s_ready = xQueueCreate(kVideoSlots, sizeof(int));
}

bool video_pacing_codec_supported(CodecId codec) {
    return codec == CodecId::Mjpeg || codec == CodecId::H264 || codec == CodecId::Mpeg2;
}

bool video_pacing_open(const MediaInfo &info, std::string *error) {
    if (!video_presenter_open_stream(info.video, error)) return false;
    s_nal_length_size = info.video.nal_length_size;
    cancel_skip();
    int64_t key_us = 0;
    s_keyframe_index = player_core.demuxer->keyframeBefore(info.duration_us, &key_us);
    video_presenter_set_source_rotation(info.video.rotation);
    return true;
}

bool video_pacing_enqueue(const Packet &packet) {
    int slot = -1;
    if (!player_take_slot(s_free, &slot)) return false;
    s_video[slot] = { packet.data, packet.len, packet.pts_us, packet.ref, 0, packet.keyframe,
                      droppable_packet(packet) };
    xQueueSend(s_ready, &slot, portMAX_DELAY);
    return true;
}

void video_pacing_drain_queues() {
    int slot = -1;
    while (xQueueReceive(s_free, &slot, 0) == pdTRUE) {
    }
    while (xQueueReceive(s_ready, &slot, 0) == pdTRUE) {
    }
}

void video_pacing_arm_slots() {
    const int slots = player_core.video_codec == CodecId::Mjpeg ? kMjpegVideoSlots : kVideoSlots;
    for (int i = 0; i < kVideoSlots; i++) {
        s_video[i] = {};
        if (i < slots) xQueueSend(s_free, &i, 0);
    }
}

void video_pacing_stop() {
    video_presenter_flush();

    if (s_have_pending) {
        slot_release(s_pending_slot);
        s_have_pending = false;
    }
    forget_repaint();
}

void video_pacing_reset_timeline() {
    s_reorder_lead_us = 0;
    s_max_pts_us = INT64_MIN;
    s_seek_frame = false;
}

void video_pacing_rewound() {
    cancel_skip();
    s_max_pts_us = INT64_MIN;
    s_seek_frame = player_core.video_codec == CodecId::Mjpeg;
}

bool video_pacing_backlog() {
    return uxQueueMessagesWaiting(s_free) == 0;
}

void video_pacing_set_poster(bool want) {
    s_want_poster = want;
}

bool video_pacing_wants_poster() {
    return s_want_poster;
}

void video_pacing_repaint() {
    if (!video_presenter_needs_source() || s_repaint_slot < 0) return;
    if (player_core.state == PlayerState::Playing) return;
    submit(s_repaint_slot, true, 0);
}

void video_pacing_step_poster() {
    int slot = -1;
    if (xQueueReceive(s_ready, &slot, pdMS_TO_TICKS(20)) != pdTRUE) return;
    slot_acquire(slot);
    keep_for_repaint(slot);
    take_seek_frame(slot);
    if (!before(s_video[slot].pts_us, player_core.next_us)) {
        show(slot);
        video_presenter_drain();
    }
    slot_release(slot);
}

void video_pacing_step() {
    if (!s_have_pending) {
        const bool drained = player_core.reader_eof;
        if (xQueueReceive(s_ready, &s_pending_slot, pdMS_TO_TICKS(20)) != pdTRUE) {
            if (drained) {
                video_presenter_drain();
                player_reached_end();
            }
            return;
        }
        slot_acquire(s_pending_slot);
        keep_for_repaint(s_pending_slot);
        take_seek_frame(s_pending_slot);
        s_have_pending = true;
    }

    const int64_t pts = s_video[s_pending_slot].pts_us;
    if (pts > s_max_pts_us) {
        s_max_pts_us = pts;
    } else if (s_max_pts_us - pts > s_reorder_lead_us) {
        s_reorder_lead_us =
            std::min(s_max_pts_us - pts, player_core.interval_us * kMaxReorderFrames);
    }
    if (before(pts, player_core.origin_pts_us)) {
        s_have_pending = false;
        if (interframe_codec()) submit(s_pending_slot, false, 0);
        slot_release(s_pending_slot);
        return;
    }
    const int64_t due = pts - player_core.origin_pts_us;
    const int64_t now = player_media_clock_us();
    const int slot = s_pending_slot;
    const VideoSlot &frame = s_video[slot];
    if (s_skip_to_keyframe && (!frame.keyframe || before(pts, s_skip_until_us))) {
        s_have_pending = false;
        xSemaphoreTake(player_core.lock, portMAX_DELAY);
        player_core.next_us = pts + player_core.interval_us;
        xSemaphoreGive(player_core.lock);
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
    const bool last = pts + player_core.interval_us >= player_core.duration_us ||
                      (player_core.reader_eof && uxQueueMessagesWaiting(s_ready) == 0);
    const bool resuming = s_skip_to_keyframe;
    cancel_skip();
    if (!last && now > due + player_core.interval_us) {
        xSemaphoreTake(player_core.lock, portMAX_DELAY);
        player_core.next_us = pts + player_core.interval_us;
        xSemaphoreGive(player_core.lock);
        if (interframe_codec() && !frame.droppable) {
            const int64_t late = now - due;
            const bool backlog = video_pacing_backlog();
            const int64_t limit =
                backlog ? player_core.interval_us * kBacklogSkipIntervals
                        : std::max(kKeyframeSkipUs,
                                   player_core.interval_us * kKeyframeSkipIntervals);
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
