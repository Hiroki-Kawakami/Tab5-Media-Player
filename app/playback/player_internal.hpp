/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "player.hpp"
#include "media/demuxer.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include <atomic>
#include <memory>
#include <string>

struct PlayerCore {
    SemaphoreHandle_t lock = nullptr;
    std::unique_ptr<Demuxer> demuxer;

    PlayerState state = PlayerState::Idle;
    std::string error;
    bool loop = false;
    bool reader_active = false;
    std::atomic<bool> reader_eof{false};

    CodecId video_codec = CodecId::None;
    int64_t duration_us = 0;
    int64_t interval_us = 0;
    int64_t shown_us = 0;
    int64_t next_us = 0;
    int64_t origin_us = 0;
    int64_t origin_pts_us = 0;
    uint64_t audio_origin_us = 0;
};

extern PlayerCore player_core;

void player_set_state(PlayerState state, const std::string &error = {});
bool player_take_slot(QueueHandle_t queue, int *slot);
int64_t player_media_clock_us();
void player_audio_stop();
void player_reached_end();

void video_pacing_start();
bool video_pacing_codec_supported(CodecId codec);
bool video_pacing_open(const MediaInfo &info, std::string *error);
bool video_pacing_enqueue(const Packet &packet);
void video_pacing_drain_queues();
void video_pacing_arm_slots();
void video_pacing_stop();
void video_pacing_reset_timeline();
void video_pacing_rewound();
bool video_pacing_backlog();
void video_pacing_set_poster(bool want);
bool video_pacing_wants_poster();
void video_pacing_repaint();
void video_pacing_step();
void video_pacing_step_poster();
