/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "file_picker_screen.hpp"
#include "media_player.hpp"
#include "screen_manager.hpp"

#include <algorithm>

static constexpr int32_t kPadding = 24;
static constexpr int32_t kButtonHeight = 72;

static FilePickerScreen *s_active;

FilePickerScreen::~FilePickerScreen() {
    if (s_active == this) s_active = nullptr;
}

void FilePickerScreen::build() {
    createNavigation("", LV_NAVIGATION_STYLE_LIST | LV_NAVIGATION_STYLE_BACK);
    back_button_ = lv_obj_get_parent(navigation_title_);

    auto footer = lv_container_create(root_, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(footer, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(footer, kPadding, 0);
    lv_obj_set_style_pad_column(footer, kPadding, 0);
    lv_obj_set_style_border_color(footer, lv_color_hex(0xc0c0c0), 0);
    lv_obj_set_style_border_width(footer, 1, 0);
    lv_obj_set_style_border_side(footer, LV_BORDER_SIDE_TOP, 0);

    auto cancel = lv_button_create(footer, LV_BUTTON_STYLE_SECONDARY);
    lv_obj_set_size(cancel, 0, kButtonHeight);
    lv_obj_set_flex_grow(cancel, 1);
    lv_button_set_text(cancel, "Cancel");
    lv_obj_add_event_fn(cancel, LV_EVENT_CLICKED, [](lv_event_t *) { screen_manager.pop(); });

    confirm_button_ = lv_button_create(footer, LV_BUTTON_STYLE_PRIMARY);
    lv_obj_set_size(confirm_button_, 0, kButtonHeight);
    lv_obj_set_flex_grow(confirm_button_, 1);
    lv_obj_set_style_opa(confirm_button_, LV_OPA_40, LV_STATE_DISABLED);
    lv_button_set_text(confirm_button_, options_.confirm);
    lv_obj_add_event_fn(confirm_button_, LV_EVENT_CLICKED,
                        [this](lv_event_t *) { pick(browser_.path()); });

    browser_.open(options_.start, options_.root);
    browser_.build(contents_);
    update_navigation();
}

void FilePickerScreen::onEnter() {
    s_active = this;
}

void FilePickerScreen::onExit() {
    if (s_active == this) s_active = nullptr;
    browser_.on_disappear();
}

void FilePickerScreen::onAppear() {
    browser_.on_appear();
}

void FilePickerScreen::onDisappear() {
    browser_.on_disappear();
}

void FilePickerScreen::back() {
    browser_.go_back();
}

void FilePickerScreen::eject(const std::string &mount_point) {
    if (!s_active || s_active->browser_.eject(mount_point)) return;
    if (screen_manager.current_screen() == s_active) screen_manager.pop();
}

void FilePickerScreen::refresh_storages() {
    if (s_active) s_active->browser_.refresh_storages();
}

void FilePickerScreen::update_navigation() {
    lv_label_set_text(navigation_title_, browser_.title().c_str());
    const bool back = browser_.can_go_back();
    lv_obj_set_hidden(lv_obj_get_child(back_button_, 0), !back);
    lv_obj_set_clickable(back_button_, back);
    const std::string &path = browser_.path();
    const bool pickable = !path.empty() && (!options_.can_pick || options_.can_pick(path));
    lv_obj_set_state(confirm_button_, LV_STATE_DISABLED, !pickable);
}

void FilePickerScreen::pick(const std::string &path) {
    if (path.empty()) return;
    auto on_pick = options_.on_pick;
    screen_manager.pop();
    if (on_pick) on_pick(path);
}

bool FilePickerScreen::is_enabled(const std::string &path, const DirectoryEntry &entry) const {
    if (!entry.directory) return options_.file_kind != MediaKind::None && entry.kind == options_.file_kind;
    return std::none_of(options_.excluded.begin(), options_.excluded.end(),
                        [&path](const std::string &excluded) { return path == excluded; });
}

void FilePickerScreen::did_open_file(std::size_t index) {
    pick(browser_.entry_path(index));
}

void FilePickerScreen::did_change_directory() {
    update_navigation();
}
