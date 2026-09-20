/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/media_types.hpp"
#include "media_buffer.h"
#include <cstdint>
#include <string>

enum class PlayerState {
    Idle,
    Loading,
    Paused,
    Playing,
    Finished,
    Failed,
};

struct PlayerStatus {
    PlayerState state = PlayerState::Idle;
    int64_t position_us = 0;
    int64_t duration_us = 0;
    bool seekable = false;
    bool loop = false;
    CodecId audio_codec = CodecId::None;
    CodecId video_codec = CodecId::None;
    std::string audio_note;
    std::string error;
};

struct MediaSummary {
    bool valid = false;
    const char *container = "";
    int64_t file_bytes = 0;
    int64_t duration_us = 0;
    bool seekable = false;
    struct {
        CodecId codec = CodecId::None;
        uint32_t width = 0;
        uint32_t height = 0;
        int64_t frame_interval_us = 0;
        bsp_rotation_t rotation = BSP_ROTATION_0;
        uint8_t profile_idc = 0;
        uint8_t level_idc = 0;
    } video;
    struct {
        CodecId codec = CodecId::None;
        uint32_t sample_rate = 0;
        uint32_t bitrate_bps = 0;
        uint8_t channels = 0;
        uint8_t bits = 0;
        std::string note;
    } audio;
};

void player_start(const media_arena_t &arena);
void player_open(const std::string &path);
void player_close();
void player_play();
void player_pause();
void player_restart();
void player_seek(int64_t position_us);
void player_set_loop(bool loop);
void player_eject(const std::string &mount_point);
void player_suspend_video(bool suspended);
void player_repaint();
PlayerStatus player_status();
MediaSummary player_media_summary();
