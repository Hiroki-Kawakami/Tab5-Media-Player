/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstdint>
#include <functional>
#include "lvgl.h"
#include "screens/home/settings_widgets.hpp"

enum PlayerSettingsSection : uint8_t {
    PlayerSettingsDisplay = 1,
    PlayerSettingsSound = 2,
};

/* The settings that can be changed while media is open, on a dark panel. The
 * root takes LV_EVENT_REFRESH to re-read the values it shares with the bar. */
void player_settings_panel_build(lv_obj_t *root, std::function<void()> on_close,
                                 uint8_t sections = PlayerSettingsDisplay | PlayerSettingsSound);
/* The sections alone, for a page the caller styles. Without `volume` the Sound
 * section holds only the Equalizer. */
void player_settings_sections_build(lv_obj_t *contents, uint8_t sections,
                                    const SettingColors *colors, bool volume);

struct PlayerVolumeRows {
    lv_obj_t *row;
    lv_obj_t *slider;
    lv_obj_t *value;
};

/* The Volume row and its slider, kept in step with the headphone jack. */
PlayerVolumeRows player_volume_rows_build(lv_obj_t *section);
