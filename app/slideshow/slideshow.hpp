/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/image_codec.hpp"
#include "media/image_pixels.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

using SlideshowFinished =
    std::function<void(std::size_t index, std::shared_ptr<const ImagePixels> pixels)>;

/* Hides the LVGL display and takes the framebuffers until the slideshow ends.
   `first` is what is on screen for `index`, or null. `interval_ms` counts from
   when a picture is shown. `on_finished` runs on the
   LVGL thread just before the display is shown again, with the picture that
   was on screen (null if none was). */
bool slideshow_start(std::vector<std::string> paths, std::size_t index, ImageBox box,
                     std::shared_ptr<const ImagePixels> first, uint32_t interval_ms,
                     SlideshowFinished on_finished);
/* Returns at once; the end runs as it does after a touch. */
void slideshow_stop();
