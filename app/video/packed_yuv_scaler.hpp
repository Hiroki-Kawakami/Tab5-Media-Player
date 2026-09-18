/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstdint>
#include <string>
#include "render_target.hpp"
#include "driver/ppa.h"

struct PackedYuvImage {
    const uint8_t *packed = nullptr;
    uint16_t coded_width = 0;
    uint16_t coded_height = 0;
    uint16_t crop_left = 0;
    uint16_t crop_top = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    bool full_range = false;
    bool bt709 = false;
};

class PackedYuvScaler {
public:
    ~PackedYuvScaler() { close(); }

    bool open(bsp_pixel_format_t format, std::string *error);
    void close();
    bool draw(const PackedYuvImage &image, const RenderTarget &target, std::string *error);

private:
    ppa_client_handle_t ppa_ = nullptr;
    ppa_srm_color_mode_t color_mode_ = PPA_SRM_COLOR_MODE_RGB565;
};
