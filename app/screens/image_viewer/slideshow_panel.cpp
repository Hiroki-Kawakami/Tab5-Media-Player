/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "slideshow_panel.hpp"
#include "screens/player_panel.hpp"
#include "screens/home/settings_widgets.hpp"
#include "screens/video_player/settings_panel.hpp"
#include "widgets.hpp"

#include <cstddef>
#include <memory>

static constexpr int32_t kStartButtonHeight = 72;
static constexpr int32_t kPadding = 24;
static constexpr int32_t kSourceRowHeight = 48;

static constexpr uint32_t kIntervals[] = { 5, 10, 20, 30, 60, 120, 300, 600, 900, 1800, 3600 };
static constexpr const char *kIntervalOptions =
    "5 sec\n10 sec\n20 sec\n30 sec\n1 min\n2 min\n5 min\n10 min\n15 min\n30 min\n1 hour";
static constexpr const char *kTransitionOptions = "None\nFade\nWipe\nSlide In\nSlide Out\nPush";
static constexpr const char *kDirectionOptions =
    "Left to Right\nRight to Left\nTop to Bottom\nBottom to Top";

static uint32_t interval_index(uint32_t interval_s) {
    for (std::size_t i = 0; i < std::size(kIntervals); i++) {
        if (kIntervals[i] >= interval_s) return (uint32_t)i;
    }
    return (uint32_t)std::size(kIntervals) - 1;
}

static const char *bgm_source_text(const std::string &path) {
    if (path.empty()) return "None";
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path.c_str() : path.c_str() + slash + 1;
}

namespace {

struct PanelState {
    SlideshowPanelValues values;
    lv_obj_t *contents = nullptr;
    lv_obj_t *direction_separator = nullptr;
    lv_obj_t *direction_row = nullptr;
    lv_obj_t *bgm_switch = nullptr;
    lv_obj_t *source_value = nullptr;
    lv_obj_t *bgm_rows[5] = {};

    void show_direction() const {
        const bool hidden = !transition_has_direction(values.transition);
        lv_obj_set_flag(direction_separator, LV_OBJ_FLAG_HIDDEN, hidden);
        lv_obj_set_flag(direction_row, LV_OBJ_FLAG_HIDDEN, hidden);
    }

    void show_bgm() const {
        lv_label_set_text(source_value, bgm_source_text(values.bgm_path));
        for (lv_obj_t *row : bgm_rows) lv_obj_set_flag(row, LV_OBJ_FLAG_HIDDEN, !values.bgm);
        lv_obj_update_layout(contents);
        lv_obj_readjust_scroll(contents, LV_ANIM_OFF);
    }
};

}

void image_slideshow_panel_build(lv_obj_t *root, const SlideshowPanelValues &values,
                                 std::function<void(const SlideshowPanelValues &)> on_change,
                                 std::function<void()> on_choose_bgm,
                                 std::function<void()> on_start, std::function<void()> on_close) {
    auto contents = player_panel_build(root, "Slideshow", std::move(on_close));
    auto state = std::make_shared<PanelState>();
    state->values = values;
    state->contents = contents;

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

    section = lv_setting_section_create(contents, "BGM", &kPanelColors);
    row = lv_setting_row_create(section, "Play Music");
    state->bgm_switch = lv_setting_switch_create(row, values.bgm,
                                                 [state, on_change](lv_obj_t *, bool enabled) {
        state->values.bgm = enabled;
        state->show_bgm();
        on_change(state->values);
    }, &kPanelColors);

    state->bgm_rows[0] = lv_setting_separator_create(section, &kPanelColors);
    row = lv_setting_row_create(section, "Source");
    state->bgm_rows[1] = row;
    lv_obj_set_height(row, kSourceRowHeight);
    lv_obj_set_style_pad_column(row, 8, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_fn(row, LV_EVENT_CLICKED, [on_choose_bgm](lv_event_t *) { on_choose_bgm(); });
    lv_obj_set_flex_grow(lv_obj_get_child(row, 0), 0);
    state->source_value = lv_setting_value_create(row, &kPanelColors);
    lv_obj_set_flex_grow(state->source_value, 1);
    lv_obj_set_height(state->source_value,
                      lv_font_get_line_height(lv_widgets_resolved_font(LV_WIDGETS_FONT_BODY)));
    lv_obj_set_style_text_align(state->source_value, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(state->source_value, LV_LABEL_LONG_MODE_DOTS);
    auto arrow = lv_setting_value_create(row, &kPanelColors);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);

    state->bgm_rows[2] = lv_setting_separator_create(section, &kPanelColors);
    const PlayerVolumeRows volume = player_volume_rows_build(section);
    state->bgm_rows[3] = volume.row;
    state->bgm_rows[4] = lv_obj_get_parent(volume.slider);
    state->show_bgm();

    lv_obj_add_event_fn(root, LV_EVENT_REFRESH, [state](lv_event_t *event) {
        const auto *values = static_cast<const SlideshowPanelValues *>(lv_event_get_param(event));
        if (!values) return;
        state->values.bgm = values->bgm;
        state->values.bgm_path = values->bgm_path;
        lv_obj_set_state(state->bgm_switch, LV_STATE_CHECKED, values->bgm);
        state->show_bgm();
    });

    auto footer = lv_container_create(root, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_size(footer, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_hor(footer, kPadding, 0);
    lv_obj_set_style_pad_bottom(footer, kPadding, 0);
    auto start = lv_button_create(footer, LV_BUTTON_STYLE_PRIMARY);
    lv_obj_set_size(start, lv_pct(100), kStartButtonHeight);
    lv_button_set_text(start, "Start Slideshow");
    lv_obj_add_event_fn(start, LV_EVENT_CLICKED, [on_start](lv_event_t *) { on_start(); });
}
