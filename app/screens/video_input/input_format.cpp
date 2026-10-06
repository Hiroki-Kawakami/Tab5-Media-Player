/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "input_format.hpp"
#include "screens/home/settings_widgets.hpp"
#include "screens/player_panel.hpp"
#include "video/mjpeg_renderer.hpp"
#include "widgets.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>

static constexpr uint32_t kMaxPixels = 1920 * 1080;
static constexpr uint32_t kIntervalsPerSecond = 10000000;
static constexpr uint32_t kMinInterval = kIntervalsPerSecond / 60;
static constexpr uint32_t kContinuousRates[] = { 60, 50, 30, 25, 24, 20, 15, 10, 5 };

static double rate_of(uint32_t interval) {
    return (double)kIntervalsPerSecond / interval;
}

static std::string rate_label(uint32_t interval) {
    const double rate = rate_of(interval);
    char text[24];
    if (std::fabs(rate - std::round(rate)) < 0.01) {
        snprintf(text, sizeof(text), "%ldHz", std::lround(rate));
        return text;
    }
    snprintf(text, sizeof(text), "%.2f", rate);
    std::string label = text;
    while (label.back() == '0') label.pop_back();
    return label + "Hz";
}

static std::vector<uint32_t> offered_intervals(const usb_host::UvcFrameSize &size) {
    std::vector<uint32_t> intervals;
    if (!size.intervals.empty()) {
        for (uint32_t interval : size.intervals) {
            if (interval >= kMinInterval) intervals.push_back(interval);
        }
    } else {
        for (uint32_t rate : kContinuousRates) {
            int64_t interval = std::lround((double)kIntervalsPerSecond / rate);
            if (size.step_interval) {
                const int64_t steps = std::llround((double)(interval - size.min_interval) /
                                                   size.step_interval);
                interval = size.min_interval + steps * size.step_interval;
            }
            if (interval < size.min_interval || interval > size.max_interval) continue;
            if (interval >= kMinInterval) intervals.push_back((uint32_t)interval);
        }
    }
    std::sort(intervals.begin(), intervals.end());
    intervals.erase(std::unique(intervals.begin(), intervals.end()), intervals.end());
    return intervals;
}

std::vector<InputSize> input_sizes(const std::vector<usb_host::UvcFrameSize> &sizes) {
    std::vector<InputSize> result;
    for (const usb_host::UvcFrameSize &size : sizes) {
        if ((uint32_t)size.width * size.height > kMaxPixels) continue;
        if (!MjpegRenderer::fits(size.width, size.height)) continue;
        std::vector<uint32_t> intervals = offered_intervals(size);
        if (intervals.empty()) continue;
        auto same = std::find_if(result.begin(), result.end(), [&](const InputSize &entry) {
            return entry.width == size.width && entry.height == size.height;
        });
        if (same == result.end()) {
            result.push_back({ size.width, size.height, std::move(intervals) });
            continue;
        }
        same->intervals.insert(same->intervals.end(), intervals.begin(), intervals.end());
        std::sort(same->intervals.begin(), same->intervals.end());
        same->intervals.erase(std::unique(same->intervals.begin(), same->intervals.end()),
                              same->intervals.end());
    }
    std::sort(result.begin(), result.end(), [](const InputSize &a, const InputSize &b) {
        const uint32_t pixels_a = (uint32_t)a.width * a.height;
        const uint32_t pixels_b = (uint32_t)b.width * b.height;
        return pixels_a != pixels_b ? pixels_a > pixels_b : a.width > b.width;
    });
    return result;
}

static uint32_t nearest_interval(const InputSize &size, uint32_t wanted) {
    if (!wanted) return size.intervals.front();
    return *std::min_element(size.intervals.begin(), size.intervals.end(),
                             [wanted](uint32_t a, uint32_t b) {
        return std::fabs(rate_of(a) - rate_of(wanted)) < std::fabs(rate_of(b) - rate_of(wanted));
    });
}

static int size_index(const std::vector<InputSize> &sizes, const InputFormat &format) {
    for (std::size_t i = 0; i < sizes.size(); i++) {
        if (sizes[i].width == format.width && sizes[i].height == format.height) return (int)i;
    }
    return -1;
}

InputFormat input_format_pick(const std::vector<InputSize> &sizes, const InputFormat &wanted) {
    if (sizes.empty()) return {};
    int index = size_index(sizes, wanted);
    if (index < 0) {
        const int64_t pixels = (int64_t)wanted.width * wanted.height;
        index = 0;
        for (std::size_t i = 1; i < sizes.size(); i++) {
            const int64_t best = (int64_t)sizes[index].width * sizes[index].height;
            const int64_t here = (int64_t)sizes[i].width * sizes[i].height;
            if (std::llabs(here - pixels) < std::llabs(best - pixels)) index = (int)i;
        }
    }
    const InputSize &size = sizes[index];
    return { size.width, size.height, nearest_interval(size, wanted.interval) };
}

std::string input_format_label(const InputFormat &format) {
    char text[24];
    snprintf(text, sizeof(text), "%ux%u @ ", format.width, format.height);
    return text + rate_label(format.interval);
}

namespace {

struct Panel {
    const std::vector<InputSize> *sizes = nullptr;
    InputFormatPanel callbacks;
    lv_obj_t *resolution = nullptr;
    lv_obj_t *rate = nullptr;
    lv_obj_t *aspect = nullptr;
};

}

/* LVGL drops a list that does not fit below upwards, over the video, which
 * paints over it. Keep it below and scroll it instead. */
static void open_downward(lv_obj_t *dropdown) {
    lv_obj_add_event_fn(dropdown, LV_EVENT_RELEASED, [dropdown](lv_event_t *) {
        if (!lv_dropdown_is_open(dropdown)) return;
        lv_obj_t *list = lv_dropdown_get_list(dropdown);
        lv_area_t button, area;
        lv_obj_get_coords(dropdown, &button);
        lv_obj_get_coords(list, &area);
        if (area.y1 >= button.y2) return;
        const int32_t room =
            lv_display_get_vertical_resolution(lv_obj_get_display(dropdown)) - button.y2 - 1;
        lv_obj_set_height(list, LV_SIZE_CONTENT);
        lv_obj_update_layout(list);
        lv_obj_set_height(list, std::min(lv_obj_get_height(list), room));
        lv_obj_align_to(list, dropdown, LV_ALIGN_OUT_BOTTOM_LEFT, 0, 0);
        lv_obj_update_layout(list);
        lv_obj_t *label = lv_obj_get_child(list, 0);
        const int32_t line = lv_font_get_line_height(lv_obj_get_style_text_font(label, LV_PART_MAIN)) +
                             lv_obj_get_style_text_line_space(label, LV_PART_MAIN);
        lv_obj_scroll_to_y(list, (int32_t)lv_dropdown_get_selected(dropdown) * line, LV_ANIM_OFF);
    });
}

static void show(const Panel &panel) {
    lv_setting_segmented_set_active(panel.aspect, panel.callbacks.stretched() ? 1 : 0);
    const InputFormat format = panel.callbacks.current();
    const int index = size_index(*panel.sizes, format);
    if (index < 0) return;
    const InputSize &size = (*panel.sizes)[index];
    lv_dropdown_set_selected(panel.resolution, index);
    std::string options;
    uint32_t selected = 0;
    for (std::size_t i = 0; i < size.intervals.size(); i++) {
        if (i) options += '\n';
        options += rate_label(size.intervals[i]);
        if (size.intervals[i] == format.interval) selected = i;
    }
    lv_dropdown_set_options(panel.rate, options.c_str());
    lv_dropdown_set_selected(panel.rate, selected);
}

void input_format_panel_build(lv_obj_t *root, const std::vector<InputSize> &sizes,
                              InputFormatPanel callbacks) {
    auto panel = std::make_shared<Panel>();
    panel->sizes = &sizes;
    panel->callbacks = std::move(callbacks);

    auto contents = player_panel_build(root, "Input Format", panel->callbacks.on_close);
    auto section = lv_setting_section_create(contents, nullptr, &kPanelColors);

    std::string options;
    for (std::size_t i = 0; i < sizes.size(); i++) {
        char text[16];
        snprintf(text, sizeof(text), "%s%ux%u", i ? "\n" : "", sizes[i].width, sizes[i].height);
        options += text;
    }
    auto row = lv_setting_row_create(section, "Resolution");
    panel->resolution = lv_setting_dropdown_create(row, options.c_str(), 0,
                                                   [panel](lv_obj_t *, uint32_t index) {
        const InputSize &size = (*panel->sizes)[index];
        panel->callbacks.on_select({ size.width, size.height,
                                     nearest_interval(size, panel->callbacks.current().interval) });
        show(*panel);
    }, &kPanelColors);
    open_downward(panel->resolution);

    lv_setting_separator_create(section, &kPanelColors);

    row = lv_setting_row_create(section, "Frame Rate");
    panel->rate = lv_setting_dropdown_create(row, "", 0, [panel](lv_obj_t *, uint32_t index) {
        InputFormat format = panel->callbacks.current();
        const int size = size_index(*panel->sizes, format);
        if (size < 0) return;
        format.interval = (*panel->sizes)[size].intervals[index];
        panel->callbacks.on_select(format);
        show(*panel);
    }, &kPanelColors);
    open_downward(panel->rate);

    lv_setting_separator_create(section, &kPanelColors);

    row = lv_setting_row_create(section, "Aspect Ratio");
    panel->aspect = lv_setting_segmented_create(row, { "Keep", "Stretch" }, 0,
                                                [panel](lv_obj_t *, int index) {
        panel->callbacks.on_stretch(index == 1);
        lv_setting_segmented_set_active(panel->aspect, index);
    }, &kPanelColors);

    show(*panel);
    lv_obj_add_event_fn(root, LV_EVENT_REFRESH, [panel](lv_event_t *) { show(*panel); });
}
