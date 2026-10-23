/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "lvgl.hpp"

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <string>

inline constexpr uint32_t kPlayerForegroundColor = 0x101010;
inline constexpr uint32_t kPlayerTrackColor = 0xd0d0d0;
inline constexpr uint32_t kPlayerArtworkColor = 0xdcdcdc;
inline constexpr uint32_t kPlayerArtworkIconColor = 0x9e9e9e;
inline constexpr uint32_t kPlayerSubtitleColor = 0x707070;
inline constexpr int32_t kPlayerPad = 24;
inline constexpr int32_t kPlayerIconButton = 72;
inline constexpr int32_t kPlayerArtworkSide = 552;

struct PlayerLayout {
    lv_obj_t *navigation;
    lv_obj_t *back_label;
    lv_obj_t *segment;
    lv_obj_t *pane;
    lv_obj_t *controls;
    lv_obj_t *title;
    lv_obj_t *subtitle;
};

/* Fills `root` for its current orientation: the back button, the pane switcher
   over an empty pane, and the controls column holding the title lines. The
   caller adds its rows to `controls` and its views to `pane`. */
PlayerLayout player_layout_build(lv_obj_t *root, const char *title, std::function<void()> on_back,
                                 std::initializer_list<const char *> pane_icons,
                                 std::function<void(int)> on_pane);
bool player_layout_landscape(lv_obj_t *root);
void player_segment_set_active(lv_obj_t *segment, int index);
/* Restarts the scroll from the start. */
void player_scrolling_text_set(lv_obj_t *label, const std::string &text);

lv_obj_t *player_artwork_create(lv_obj_t *pane);
/* The rounded page a pane other than the artwork shows, covering the pane. */
lv_obj_t *player_pane_view_create(lv_obj_t *pane);

struct PlayerRow {
    lv_obj_t *left;
    lv_obj_t *middle;
    lv_obj_t *right;
};

/* A controls row whose side cells line up with the time labels. */
PlayerRow player_row_build(lv_obj_t *controls, int32_t height = LV_SIZE_CONTENT);

struct PlayerSeekRow {
    lv_obj_t *elapsed;
    lv_obj_t *slider;
    lv_obj_t *total;
};

PlayerSeekRow player_seek_row_build(lv_obj_t *controls, int32_t range);
