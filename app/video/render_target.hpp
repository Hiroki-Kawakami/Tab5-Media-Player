/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <cstdint>
#include "bsp_types.h"

using VideoPresenterRelease = void (*)(void *ctx);

inline constexpr uint32_t kScaleDenominator = 16;

struct RenderTarget {
    void *framebuffer = nullptr;
    std::size_t framebuffer_bytes = 0;
    bsp_size_t panel = {};
    bsp_size_t source = {};
    bsp_rotation_t rotation = BSP_ROTATION_0;
    uint32_t scale_n = kScaleDenominator;
    bsp_rect_t rect = {};
    bsp_rect_t clip = {};
};

inline bsp_rect_t render_target_visible(const RenderTarget &target) {
    const bsp_rect_t &r = target.rect;
    const bsp_rect_t &c = target.clip;
    const int x0 = r.origin.x > c.origin.x ? r.origin.x : c.origin.x;
    const int y0 = r.origin.y > c.origin.y ? r.origin.y : c.origin.y;
    const int x1 = r.origin.x + r.size.width < c.origin.x + c.size.width ? r.origin.x + r.size.width
                                                                         : c.origin.x + c.size.width;
    const int y1 = r.origin.y + r.size.height < c.origin.y + c.size.height ? r.origin.y + r.size.height
                                                                           : c.origin.y + c.size.height;
    if (x1 <= x0 || y1 <= y0) return {};
    return { { x0, y0 }, { x1 - x0, y1 - y0 } };
}

inline bool render_target_clipped(const RenderTarget &target) {
    const bsp_rect_t visible = render_target_visible(target);
    return visible.size.width != target.rect.size.width || visible.size.height != target.rect.size.height;
}
