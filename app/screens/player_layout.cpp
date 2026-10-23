/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "player_layout.hpp"
#include "screens/media_controls.hpp"
#include "resources.h"
#include "widgets.hpp"

static constexpr int32_t kGap = 24;
static constexpr int32_t kRowGap = 16;
static constexpr int32_t kSliderReach = 24;
static constexpr int32_t kTimeWidth = 100;
static constexpr int32_t kTitleGap = 12;
static constexpr int32_t kPortraitTitleBottomPad = 8;
static constexpr int32_t kLandscapeTitleBottomPad = 20;
static constexpr int32_t kSeekRowHeight = 72;
static constexpr int32_t kHeaderHeight = 120;
static constexpr int32_t kSegmentHeight = 64;
static constexpr int32_t kSegmentGap = 16;
static constexpr int32_t kSegmentInset = 14;
static constexpr int32_t kDividerHeight = 28;
static constexpr int32_t kArtworkRadius = 24;
static constexpr uint32_t kSegmentIconColor = 0x707070;
static constexpr uint32_t kPaneColor = 0xeeeeee;
static constexpr int32_t kMarqueePauseMs = 2000;

static constexpr lv_anim_t marquee_template() {
    lv_anim_t anim{};
    anim.act_time = -kMarqueePauseMs;
    anim.repeat_cnt = LV_ANIM_REPEAT_INFINITE;
    anim.repeat_delay = kMarqueePauseMs;
    return anim;
}

/* Only the delay and the repeat are read from it. Constant so it costs no RAM. */
static constexpr lv_anim_t kMarquee = marquee_template();

static lv_obj_t *build_navigation(lv_obj_t *parent, const char *title,
                                  std::function<void()> on_back, lv_obj_t **back_label) {
    lv_obj_t *navigation = lv_navigation_create(parent, LV_NAVIGATION_STYLE_UNIFIED);
    lv_obj_t *back = lv_navigation_back_create(
        navigation, title, [on_back = std::move(on_back)](lv_event_t *) { on_back(); });
    *back_label = lv_navigation_back_label(back);
    return navigation;
}

static lv_obj_t *build_segment(lv_obj_t *parent, int32_t width,
                               std::initializer_list<const char *> icons,
                               std::function<void(int)> on_pane) {
    const lv_color_t foreground = lv_color_hex(kPlayerForegroundColor);
    const int count = (int)icons.size();
    const int32_t cell = width / count;
    const int32_t left = (width - cell * count) / 2;
    lv_obj_t *segment = lv_container_create(parent);
    lv_obj_set_size(segment, width, kSegmentHeight);
    lv_obj_set_scrollable(segment, false);

    int index = 0;
    for (const char *icon : icons) {
        lv_obj_t *button = media_toggle_button(segment, kSegmentHeight, &icon_28, icon, foreground,
                                               lv_color_white());
        lv_obj_set_size(button, cell, kSegmentHeight);
        lv_obj_set_pos(button, left + index * cell, 0);
        lv_obj_set_style_text_color(button, lv_color_hex(kSegmentIconColor), 0);
        lv_obj_add_event_fn(button, LV_EVENT_CLICKED,
                            [on_pane, index](lv_event_t *) { on_pane(index); });
        index++;
    }
    for (int i = 1; i < count; i++) {
        lv_obj_t *divider = lv_container_create(segment, lv_color_hex(kPlayerTrackColor));
        lv_obj_set_size(divider, 1, kDividerHeight);
        lv_obj_set_pos(divider, left + i * cell, (kSegmentHeight - kDividerHeight) / 2);
    }
    return segment;
}

static lv_obj_t *build_pane(lv_obj_t *parent, int32_t width) {
    lv_obj_t *pane = lv_container_create(parent);
    lv_obj_set_size(pane, width, kPlayerArtworkSide);
    lv_obj_set_scrollable(pane, false);
    return pane;
}

static lv_obj_t *build_scrolling_label(lv_obj_t *parent, lv_widgets_font_role_t role) {
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_height(label, lv_font_get_line_height(lv_widgets_resolved_font(role)));
    lv_obj_set_font_role(label, role);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_anim(label, &kMarquee, 0);
    player_scrolling_text_set(label, "");
    return label;
}

static lv_obj_t *build_controls(lv_obj_t *area, int32_t title_bottom_pad, PlayerLayout *layout) {
    lv_obj_set_flex_flow(area, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(area, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    lv_obj_t *controls = lv_container_create(area);
    lv_obj_set_size(controls, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(controls, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(controls, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(controls, kGap, 0);

    lv_obj_t *box = lv_container_create(controls);
    lv_obj_set_size(box, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(box, kTitleGap, 0);
    lv_obj_set_style_pad_bottom(box, title_bottom_pad, 0);
    layout->title = build_scrolling_label(box, LV_WIDGETS_FONT_TITLE);
    layout->subtitle = build_scrolling_label(box, LV_WIDGETS_FONT_BODY);
    lv_obj_set_style_text_color(layout->subtitle, lv_color_hex(kPlayerSubtitleColor), 0);
    return controls;
}

PlayerLayout player_layout_build(lv_obj_t *root, const char *title, std::function<void()> on_back,
                                 std::initializer_list<const char *> pane_icons,
                                 std::function<void(int)> on_pane) {
    PlayerLayout layout = {};
    lv_obj_clean(root);
    lv_obj_set_style_bg_color(root, lv_color_white(), 0);
    lv_obj_set_style_text_color(root, lv_color_hex(kPlayerForegroundColor), 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_set_style_pad_row(root, 0, 0);
    lv_obj_set_style_pad_column(root, 0, 0);

    if (!player_layout_landscape(root)) {
        lv_obj_set_flex_flow(root, LV_FLEX_FLOW_COLUMN);
        layout.navigation = build_navigation(root, title, std::move(on_back), &layout.back_label);

        lv_obj_t *contents = lv_spacer_create(root, lv_pct(100), LV_SIZE_CONTENT, 1);
        lv_obj_set_flex_flow(contents, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(contents, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_hor(contents, kPlayerPad, 0);
        lv_obj_update_layout(root);

        const int32_t width = lv_obj_get_content_width(contents);
        layout.segment =
            build_segment(contents, width - 2 * kSegmentInset, pane_icons, std::move(on_pane));
        lv_spacer_create(contents, 1, kSegmentGap);
        layout.pane = build_pane(contents, width);

        lv_obj_t *area = lv_spacer_create(contents, lv_pct(100), LV_SIZE_CONTENT, 1);
        layout.controls = build_controls(area, kPortraitTitleBottomPad, &layout);
        return layout;
    }

    lv_obj_set_flex_flow(root, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_right(root, kPlayerPad, 0);

    lv_obj_t *left = lv_spacer_create(root, LV_SIZE_CONTENT, lv_pct(100), 1);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_COLUMN);
    layout.navigation = build_navigation(left, title, std::move(on_back), &layout.back_label);
    lv_obj_t *area = lv_spacer_create(left, lv_pct(100), LV_SIZE_CONTENT, 1);
    lv_obj_set_style_pad_hor(area, kPlayerPad, 0);
    layout.controls = build_controls(area, kLandscapeTitleBottomPad, &layout);

    lv_obj_t *right = lv_container_create(root);
    lv_obj_set_size(right, kPlayerArtworkSide, lv_pct(100));
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(right, (kHeaderHeight - kSegmentHeight) / 2, 0);
    lv_obj_set_style_pad_bottom(right, kPlayerPad, 0);
    layout.segment = build_segment(right, kPlayerArtworkSide, pane_icons, std::move(on_pane));
    lv_spacer_create(right, 1, kSegmentGap);
    layout.pane = build_pane(right, kPlayerArtworkSide);
    lv_obj_set_flex_grow(layout.pane, 1);
    return layout;
}

bool player_layout_landscape(lv_obj_t *root) {
    return lv_obj_get_width(root) > lv_obj_get_height(root);
}

void player_segment_set_active(lv_obj_t *segment, int index) {
    const int count = ((int)lv_obj_get_child_count(segment) + 1) / 2;
    for (int i = 0; i < count; i++) {
        lv_obj_set_state(lv_obj_get_child(segment, i), LV_STATE_CHECKED, i == index);
    }
    for (int i = 1; i < count; i++) {
        lv_obj_set_hidden(lv_obj_get_child(segment, count + i - 1), i == index || i - 1 == index);
    }
}

void player_scrolling_text_set(lv_obj_t *label, const std::string &text) {
    /* Setting the mode again restarts the scroll from the start; a plain
     * set_text keeps the old offset. */
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_SCROLL_CIRCULAR);
    lv_label_set_text(label, text.c_str());
}

lv_obj_t *player_artwork_create(lv_obj_t *pane) {
    lv_obj_t *artwork = lv_container_create(pane, lv_color_hex(kPlayerArtworkColor));
    lv_obj_set_size(artwork, kPlayerArtworkSide, kPlayerArtworkSide);
    lv_obj_set_style_radius(artwork, kArtworkRadius, 0);
    lv_obj_set_style_clip_corner(artwork, true, 0);
    lv_obj_set_scrollable(artwork, false);
    lv_obj_align(artwork, LV_ALIGN_BOTTOM_MID, 0, 0);
    return artwork;
}

lv_obj_t *player_pane_view_create(lv_obj_t *pane) {
    lv_obj_t *view = lv_container_create(pane, lv_color_hex(kPaneColor));
    lv_obj_set_size(view, lv_pct(100), lv_pct(100));
    lv_obj_set_style_radius(view, kArtworkRadius, 0);
    lv_obj_set_style_clip_corner(view, true, 0);
    lv_obj_set_flex_flow(view, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(view, LV_DIR_VER);
    return view;
}

static lv_obj_t *side_cell(lv_obj_t *row) {
    lv_obj_t *cell = lv_container_create(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(cell, kTimeWidth, LV_SIZE_CONTENT);
    return cell;
}

PlayerRow player_row_build(lv_obj_t *controls, int32_t height) {
    lv_obj_t *row = lv_container_create(controls, LV_FLEX_FLOW_ROW);
    lv_obj_set_height(row, height);
    lv_obj_set_style_pad_column(row, kRowGap, 0);

    PlayerRow cells;
    cells.left = side_cell(row);
    cells.middle = lv_container_create(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_grow(cells.middle, 1);
    lv_obj_set_style_pad_column(cells.middle, kRowGap, 0);
    /* As tall as a slider track. Its knob and touch pad reach past that, and a
       parent clips drawing and touches to its own ext draw size. */
    lv_obj_set_overflow_visible(cells.middle, true);
    lv_obj_add_event_fn(cells.middle, LV_EVENT_REFR_EXT_DRAW_SIZE,
                        [](lv_event_t *event) { lv_event_set_ext_draw_size(event, kSliderReach); });
    cells.right = side_cell(row);
    return cells;
}

static lv_obj_t *time_label(lv_obj_t *cell) {
    lv_obj_t *label = lv_label_create(cell);
    lv_obj_set_width(label, lv_pct(100));
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_font_role(label, LV_WIDGETS_FONT_BODY);
    return label;
}

PlayerSeekRow player_seek_row_build(lv_obj_t *controls, int32_t range) {
    const PlayerRow row = player_row_build(controls, kSeekRowHeight);
    PlayerSeekRow seek;
    seek.elapsed = time_label(row.left);
    seek.slider = media_slider(row.middle, range, lv_color_hex(kPlayerForegroundColor),
                               lv_color_hex(kPlayerTrackColor));
    lv_obj_set_flex_grow(seek.slider, 1);
    seek.total = time_label(row.right);
    return seek;
}
