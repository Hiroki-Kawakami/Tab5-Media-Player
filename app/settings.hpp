/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <functional>
#include <string>
#include "bsp.h"
#include "lvgl.h"
#include "playback/playlist.hpp"
#include "slideshow/transition.hpp"

inline constexpr int kMinDisplayBrightness = 1;
inline constexpr int kDefaultSpeakerVolume = 60;
inline constexpr int kDefaultHeadphoneVolume = 40;
inline constexpr int kMinSlideshowInterval = 5;
inline constexpr int kMaxSlideshowInterval = 3600;

void settings_init();
void settings_apply();
void settings_commit();

int settings_display_brightness();
void settings_set_display_brightness(int percent);

bsp_pixel_format_t settings_display_pixel_format();
esp_err_t settings_set_display_pixel_format(bsp_pixel_format_t format);

bool settings_rotation_locked();
void settings_set_rotation_lock(bool locked);
bsp_rotation_t settings_rotation();
void settings_set_rotation(bsp_rotation_t rotation);

bool settings_volume_is_headphone();
int settings_volume();
void settings_set_volume(int percent);
void settings_volume_observe(lv_obj_t *owner, std::function<void(int)> on_change);

bool settings_equalizer_enabled();
void settings_set_equalizer_enabled(bool enabled);

RepeatMode settings_audio_repeat();
void settings_set_audio_repeat(RepeatMode mode);

bool settings_audio_shuffle();
void settings_set_audio_shuffle(bool enabled);

RepeatMode settings_video_repeat();
void settings_set_video_repeat(RepeatMode mode);

bool settings_video_shuffle();
void settings_set_video_shuffle(bool enabled);

int settings_slideshow_interval();
void settings_set_slideshow_interval(int seconds);

bool settings_slideshow_shuffle();
void settings_set_slideshow_shuffle(bool enabled);

TransitionKind settings_slideshow_transition();
void settings_set_slideshow_transition(TransitionKind kind);

TransitionDirection settings_slideshow_direction();
void settings_set_slideshow_direction(TransitionDirection direction);

bool settings_slideshow_hold_to_exit();
void settings_set_slideshow_hold_to_exit(bool enabled);

bool settings_slideshow_bgm();
void settings_set_slideshow_bgm(bool enabled);

bool settings_slideshow_bgm_shuffle();
void settings_set_slideshow_bgm_shuffle(bool enabled);

const std::string &settings_slideshow_bgm_path();
void settings_set_slideshow_bgm_path(const std::string &path);
