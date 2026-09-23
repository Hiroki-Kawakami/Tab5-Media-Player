/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "slideshow_panel.hpp"
#include "screens/player_panel.hpp"
#include "screens/home/settings_widgets.hpp"
#include "widgets.hpp"

#include <cstddef>
#include <memory>

static constexpr int32_t kStartButtonHeight = 72;

static constexpr uint32_t kIntervals[] = { 5, 10, 20, 30, 60, 120, 300, 600, 900, 1800, 3600 };
static constexpr const char *kIntervalOptions =
    "5 sec\n10 sec\n20 sec\n30 sec\n1 min\n2 min\n5 min\n10 min\n15 min\n30 min\n1 hour";
static constexpr const char *kTransitionOptions = "None\nFade\nWipe\nSlide In";
static constexpr const char *kDirectionOptions =
    "Left to Right\nRight to Left\nTop to Bottom\nBottom to Top";

static uint32_t interval_index(uint32_t interval_s) {
    for (std::size_t i = 0; i < std::size(kIntervals); i++) {
        if (kIntervals[i] >= interval_s) return (uint32_t)i;
    }
    return (uint32_t)std::size(kIntervals) - 1;
}

namespace {

struct PanelState {
    SlideshowPanelValues values;
    lv_obj_t *direction_separator = nullptr;
    lv_obj_t *direction_row = nullptr;

    void show_direction() const {
        const bool hidden = !transition_has_direction(values.transition);
        lv_obj_set_flag(direction_separator, LV_OBJ_FLAG_HIDDEN, hidden);
        lv_obj_set_flag(direction_row, LV_OBJ_FLAG_HIDDEN, hidden);
    }
};

}

void image_slideshow_panel_build(lv_obj_t *root, const SlideshowPanelValues &values,
                                 std::function<void(const SlideshowPanelValues &)> on_change,
                                 std::function<void()> on_start, std::function<void()> on_close) {
    auto contents = player_panel_build(root, "Slideshow", std::move(on_close));
    auto state = std::make_shared<PanelState>();
    state->values = values;

    auto section = lv_setting_section_create(contents, nullptr, &kPanelColors);
    auto row = lv_setting_row_create(section, "Interval");
    lv_setting_dropdown_create(row, kIntervalOptions, interval_index(values.interval_s),
                               [state, on_change](lv_obj_t *, uint32_t index) {
        state->values.interval_s = kIntervals[index];
        on_change(state->values);
    }, &kPanelColors);

    lv_setting_separator_create(section, &kPanelColors);
    row = lv_setting_row_create(section, "Transition");
    lv_setting_dropdown_create(row, kTransitionOptions, (uint32_t)values.transition,
                               [state, on_change](lv_obj_t *, uint32_t index) {
        state->values.transition = (TransitionKind)index;
        state->show_direction();
        on_change(state->values);
    }, &kPanelColors);

    state->direction_separator = lv_setting_separator_create(section, &kPanelColors);
    state->direction_row = lv_setting_row_create(section, "Direction");
    lv_setting_dropdown_create(state->direction_row, kDirectionOptions, (uint32_t)values.direction,
                               [state, on_change](lv_obj_t *, uint32_t index) {
        state->values.direction = (TransitionDirection)index;
        on_change(state->values);
    }, &kPanelColors);
    state->show_direction();

    auto start = lv_button_create(contents, LV_BUTTON_STYLE_PRIMARY);
    lv_obj_set_size(start, lv_pct(100), kStartButtonHeight);
    lv_button_set_text(start, "Start Slideshow");
    lv_obj_add_event_fn(start, LV_EVENT_CLICKED, [on_start](lv_event_t *) { on_start(); });
}
