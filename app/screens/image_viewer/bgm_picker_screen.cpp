/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "bgm_picker_screen.hpp"
#include "media_player.hpp"
#include "screen_manager.hpp"
#include "bsp.h"
#include "usb_msc.h"

#include <cstring>
#include <sys/stat.h>

static constexpr int32_t kRowHeight = 80;
static constexpr int32_t kIconWidth = 56;
static constexpr int32_t kPadding = 24;
static constexpr int32_t kButtonHeight = 72;

static constexpr const char *kStorages[] = { kSdMountPoint, kUsbMountPoint };

static BgmPickerScreen *s_active;

static const char *storage_name(const char *mount_point) {
    return strcmp(mount_point, kUsbMountPoint) == 0 ? "USB Drive" : "SD Card";
}

static bool storage_mounted(const char *mount_point) {
    return strcmp(mount_point, kUsbMountPoint) == 0 ? usb_msc_is_mounted() : bsp_sd_is_mounted();
}

static const char *mount_point_of(const std::string &path) {
    for (const char *mount_point : kStorages) {
        if (path_is_under(path, mount_point) && storage_mounted(mount_point)) return mount_point;
    }
    return nullptr;
}

static std::string parent_of(const std::string &path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos || slash == 0 ? std::string() : path.substr(0, slash);
}

BgmPickerScreen::~BgmPickerScreen() {
    if (s_active == this) s_active = nullptr;
}

void BgmPickerScreen::build() {
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

    use_button_ = lv_button_create(footer, LV_BUTTON_STYLE_PRIMARY);
    lv_obj_set_size(use_button_, 0, kButtonHeight);
    lv_obj_set_flex_grow(use_button_, 1);
    lv_obj_set_style_opa(use_button_, LV_OPA_40, LV_STATE_DISABLED);
    lv_button_set_text(use_button_, "Use This Folder");
    lv_obj_add_event_fn(use_button_, LV_EVENT_CLICKED, [this](lv_event_t *) { pick(path_); });

    std::string start = start_;
    struct stat st;
    while (!start.empty() && (!mount_point_of(start) || stat(start.c_str(), &st) != 0 ||
                              !S_ISDIR(st.st_mode))) {
        start = parent_of(start);
    }
    show(start);
}

void BgmPickerScreen::onEnter() {
    s_active = this;
}

void BgmPickerScreen::onExit() {
    if (s_active == this) s_active = nullptr;
}

void BgmPickerScreen::back() {
    if (path_.empty()) return;
    const char *mount_point = mount_point_of(path_);
    if (!mount_point || path_ == mount_point) {
        navigate({});
    } else {
        navigate(parent_of(path_));
    }
}

void BgmPickerScreen::eject(const std::string &mount_point) {
    if (s_active && path_is_under(s_active->path_, mount_point)) s_active->navigate({});
}

void BgmPickerScreen::navigate(std::string path) {
    std::weak_ptr<Screen> weak = weak_from_this();
    lv_async_call([this, weak, path = std::move(path)] {
        if (!weak.expired() && !exited()) show(path);
    });
}

void BgmPickerScreen::show(const std::string &path) {
    path_ = path;
    entries_.clear();
    lv_obj_clean(contents_);
    lv_obj_set_style_bg_color(contents_, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(contents_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(contents_, LV_OBJ_FLAG_SCROLLABLE);

    bool opened = true;
    if (path_.empty()) {
        lv_label_set_text(navigation_title_, "Storage");
        for (const char *mount_point : kStorages) {
            entries_.push_back({ PsramString(storage_name(mount_point)), true, MediaKind::None });
        }
    } else {
        const char *mount_point = mount_point_of(path_);
        lv_label_set_text(navigation_title_, mount_point && path_ == mount_point
                                                 ? storage_name(mount_point)
                                                 : path_.c_str() + path_.rfind('/') + 1);
        PsramVector<DirectoryEntry> all;
        opened = media_directory_list(path_, &all);
        for (DirectoryEntry &entry : all) {
            if (entry.directory || entry.kind == MediaKind::Audio) entries_.push_back(std::move(entry));
        }
    }
    lv_obj_set_state(use_button_, LV_STATE_DISABLED, path_.empty());
    lv_obj_set_flag(lv_obj_get_child(back_button_, 0), LV_OBJ_FLAG_HIDDEN, path_.empty());
    lv_obj_set_flag(back_button_, LV_OBJ_FLAG_CLICKABLE, !path_.empty());

    if (!opened || entries_.empty()) {
        lv_obj_set_flex_align(contents_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        auto label = lv_label_create(contents_);
        lv_label_set_text(label, opened ? "No music" : "Cannot open directory");
        lv_obj_set_font_role(label, LV_WIDGETS_FONT_BODY);
        lv_obj_set_style_text_color(label, lv_color_hex(0x808080), 0);
        return;
    }
    lv_obj_set_flex_align(contents_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_list_create(contents_, this);
}

void BgmPickerScreen::pick(const std::string &path) {
    if (path.empty()) return;
    auto on_pick = on_pick_;
    screen_manager.pop();
    if (on_pick) on_pick(path);
}

std::string BgmPickerScreen::entryPath(std::size_t index) const {
    if (path_.empty()) return kStorages[index];
    return path_ + "/" + entries_[index].name.c_str();
}

std::size_t BgmPickerScreen::rowCount() const {
    return entries_.size();
}

int32_t BgmPickerScreen::rowHeight() const {
    return kRowHeight;
}

lv_obj_t *BgmPickerScreen::createRow(lv_obj_t *parent) {
    lv_obj_t *row = lv_list_row_create(parent);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 24, 0);

    lv_obj_t *icon = lv_label_create(row);
    lv_obj_set_width(icon, kIconWidth);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_font_role(icon, LV_WIDGETS_FONT_BODY);

    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_flex_grow(name, 1);
    lv_obj_set_height(name, lv_font_get_line_height(lv_widgets_resolved_font(LV_WIDGETS_FONT_BODY)));
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_font_role(name, LV_WIDGETS_FONT_BODY);

    lv_obj_t *arrow = lv_label_create(row);
    lv_obj_set_width(arrow, 48);
    lv_obj_set_style_text_align(arrow, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_font_role(arrow, LV_WIDGETS_FONT_BODY);
    lv_obj_set_style_text_color(arrow, lv_color_hex(0x808080), 0);

    lv_obj_set_style_pad_hor(row, 24, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_t *separator = lv_hor_separator_create(row);
    lv_obj_add_flag(separator, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_remove_flag(separator, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_align(separator, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
    return row;
}

void BgmPickerScreen::bindRow(lv_obj_t *row, std::size_t index) {
    const DirectoryEntry &entry = entries_[index];
    const char *icon = !path_.empty()
        ? (entry.directory ? LV_SYMBOL_DIRECTORY : LV_SYMBOL_AUDIO)
        : (strcmp(kStorages[index], kUsbMountPoint) == 0 ? LV_SYMBOL_USB : LV_SYMBOL_SD_CARD);
    lv_label_set_text(lv_obj_get_child(row, 0), icon);
    lv_label_set_text(lv_obj_get_child(row, 1), entry.name.c_str());
    lv_obj_set_flag(lv_obj_get_child(row, 2), LV_OBJ_FLAG_HIDDEN, !entry.directory);
}

void BgmPickerScreen::openStorage(const char *mount_point) {
    const bool usb = strcmp(mount_point, kUsbMountPoint) == 0;
    const esp_err_t err = usb ? media_player_mount_usb() : media_player_mount_sd();
    if (err == ESP_OK) {
        navigate(mount_point);
        return;
    }
    std::string text = usb && err == ESP_ERR_NOT_FOUND ? "No USB drive connected"
                     : usb                              ? "Failed to mount USB drive"
                                                        : "Failed to mount SD card";
    if (err != ESP_ERR_NOT_FOUND) text = text + "\n" + esp_err_to_name(err);
    auto modal = lv_modal_open(root_);
    lv_modal_title_create(modal, storage_name(mount_point));
    lv_modal_message_create(modal, text.c_str());
    lv_modal_button_create(modal, "Close", LV_MODAL_BUTTON_TYPE_PRIMARY,
                           [modal](lv_event_t *) { lv_modal_close(modal); });
}

void BgmPickerScreen::didSelectRow(std::size_t index) {
    if (path_.empty()) {
        openStorage(kStorages[index]);
    } else if (entries_[index].directory) {
        navigate(entryPath(index));
    } else {
        pick(entryPath(index));
    }
}
