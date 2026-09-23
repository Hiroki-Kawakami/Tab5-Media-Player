/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/image_codec.hpp"
#include "playback/playlist.hpp"
#include "slideshow/transition.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

struct SlideshowConfig {
    /* Counts from when a transition has finished. */
    uint32_t interval_ms = 5000;
    TransitionKind transition = TransitionKind::None;
    TransitionDirection direction = TransitionDirection::LeftToRight;
    TransitionCurve curve = TransitionCurve::Linear;
    bool shuffle = false;
    bool bgm_shuffle = false;
};

using SlideshowFinished = std::function<void(std::size_t index)>;

/* Hides the LVGL display and takes the framebuffers until the slideshow ends,
   and plays `bgm` in a loop through the player meanwhile. `on_finished` runs
   on the LVGL thread just before the display is shown again, with the index
   of the picture that was on screen. */
bool slideshow_start(std::vector<PlaylistItem> pictures, std::size_t index, ImageSize box,
                     const SlideshowConfig &config, std::vector<PlaylistItem> bgm,
                     SlideshowFinished on_finished);
/* Returns at once; the end runs as it does after a touch. */
void slideshow_stop();
