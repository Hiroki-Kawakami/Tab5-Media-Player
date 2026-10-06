/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>
#include "lvgl.h"
#include "usb_host_uvc.hpp"

struct InputFormat {
    uint16_t width = 0;
    uint16_t height = 0;
    uint32_t interval = 0;

    bool operator==(const InputFormat &) const = default;
};

struct InputSize {
    uint16_t width = 0;
    uint16_t height = 0;
    std::vector<uint32_t> intervals;  // fastest first
};

/* The camera's sizes the player shows: up to 1920x1080 worth of pixels the
 * decoder takes, at up to 60 Hz. Largest first. */
std::vector<InputSize> input_sizes(const std::vector<usb_host::UvcFrameSize> &sizes);
/* The offered format nearest `wanted`; zero width when nothing is offered. */
InputFormat input_format_pick(const std::vector<InputSize> &sizes, const InputFormat &wanted);
std::string input_format_label(const InputFormat &format);

struct InputFormatPanel {
    std::function<InputFormat()> current;
    std::function<void(const InputFormat &)> on_select;
    std::function<bool()> stretched;
    std::function<void(bool)> on_stretch;
    std::function<void()> on_close;
};

/* Resolution and Frame Rate dropdowns and the Aspect Ratio choice on a dark
 * panel. The root takes LV_EVENT_REFRESH to show the current values again. */
void input_format_panel_build(lv_obj_t *root, const std::vector<InputSize> &sizes,
                              InputFormatPanel callbacks);
