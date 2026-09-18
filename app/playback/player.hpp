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

void player_start(const media_arena_t &arena);
void player_open(const std::string &path);
void player_close();
void player_play();
void player_pause();
void player_restart();
void player_seek(int64_t position_us);
void player_set_loop(bool loop);
void player_eject(const std::string &mount_point);
PlayerStatus player_status();
