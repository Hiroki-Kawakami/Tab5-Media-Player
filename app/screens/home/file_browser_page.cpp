/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "file_browser_page.hpp"
#include "screens/home_screen.hpp"
#include "media/media_cache.hpp"
#include "media_player.hpp"
#include "resources.h"
#include "screen_manager.hpp"
#include "screens/audio_player_screen.hpp"
#include "screens/file_picker_screen.hpp"
#include "screens/image_viewer_screen.hpp"
#include "screens/video_player_screen.hpp"
#include "esp_log.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <strings.h>
#include <sys/stat.h>

static const char *TAG = "file_browser";

static constexpr uint32_t kResolveTimeoutMs = 700;
static constexpr int32_t kToolbarPadding = 24;
static constexpr int32_t kToolbarButtonHeight = 72;
static constexpr int32_t kModalPadding = 16;
static constexpr uint32_t kProgressPeriodMs = 100;
static constexpr const char *kInvalidNameChars = "/\\:*?\"<>|";

static std::string name_of(const std::string &path) {
    return path.substr(path.rfind('/') + 1);
}

static std::size_t utf8_length(const std::string &text) {
    std::size_t length = 0;
    for (unsigned char c : text) {
        if ((c & 0xc0) != 0x80) length++;
    }
    return length;
}

static bool exists(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0;
}

static bool is_directory(const std::string &path) {
    struct stat st;
    return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

static std::string unused_name(const std::string &directory, const std::string &name, bool folder) {
    std::size_t dot = folder ? std::string::npos : name.rfind('.');
    if (dot == 0) dot = std::string::npos;
    const std::string stem = name.substr(0, dot);
    const std::string extension = dot == std::string::npos ? "" : name.substr(dot);
    for (int n = 2;; n++) {
        std::string candidate = stem + " (" + std::to_string(n) + ")" + extension;
        if (!exists(directory + "/" + candidate)) return candidate;
    }
}

FileBrowserPage::FileBrowserPage(const std::string &path) {
    browser_.open(path, path);
}

FileBrowserPage::~FileBrowserPage() {
    close_progress();
    close_rename();
}

const std::string &FileBrowserPage::title() const {
    return browser_.selecting() ? selection_title_ : browser_.title();
}

void FileBrowserPage::post(std::function<void(FileBrowserPage &)> fn) {
    lv_async_call([weak = weak_from_this(), fn = std::move(fn)] {
        if (auto self = weak.lock()) fn(*self);
    });
}

void FileBrowserPage::build_navigation(lv_obj_t *navigation) {
    lv_obj_t *actions = lv_navigation_actions(navigation);
    if (browser_.selecting()) {
        lv_obj_set_style_pad_column(actions, kToolbarPadding, 0);
        auto add = [actions](const char *text, lv_button_style_t style) {
            auto button = lv_button_create(actions, style);
            lv_obj_set_height(button, kToolbarButtonHeight);
            lv_obj_set_style_pad_hor(button, kToolbarPadding, 0);
            lv_button_set_text(button, text);
            return button;
        };
        const bool deselect = browser_.selected_count() == browser_.entries().size();
        auto all = add(deselect ? "Deselect All" : "Select All", LV_BUTTON_STYLE_SECONDARY);
        lv_obj_add_event_fn(all, LV_EVENT_CLICKED,
                            [this, deselect](lv_event_t *) { browser_.select_all(!deselect); });
        auto done = add("Done", LV_BUTTON_STYLE_PRIMARY);
        lv_obj_add_event_fn(done, LV_EVENT_CLICKED, [this](lv_event_t *) { set_selecting(false); });
        return;
    }
    auto button = lv_button_create(actions, LV_BUTTON_STYLE_NAVIGATION);
    lv_button_set_text(button, TABLER_DOTS, &icon_36);
    lv_obj_add_event_fn(button, LV_EVENT_CLICKED, [this, button](lv_event_t *) { open_menu(button); });
}

void FileBrowserPage::open_menu(lv_obj_t *anchor) {
    struct LayoutItem {
        BrowserLayout layout;
        const char *icon;
        const char *label;
    };
    static constexpr LayoutItem kLayouts[] = {
        {BrowserLayout::Compact, TABLER_LIST, "Compact"},
        {BrowserLayout::List, TABLER_LIST_DETAILS, "List"},
        {BrowserLayout::Grid, TABLER_LAYOUT_GRID, "Grid"},
    };

    auto popover = lv_popover_open(anchor);
    lv_popover_heading_create(popover, "Layout");
    for (const LayoutItem &item : kLayouts) {
        auto row = lv_popover_row_create(popover, item.icon, &icon_36, item.label,
                                         [this, layout = item.layout](lv_event_t *) { browser_.set_layout(layout); });
        lv_popover_row_set_checked(row, settings_browser_layout() == item.layout);
    }
    lv_popover_separator_create(popover);
    lv_popover_row_create(popover, TABLER_CIRCLE_CHECK, &icon_36, "Select",
                          [this](lv_event_t *) { set_selecting(true); });
}

void FileBrowserPage::set_selecting(bool selecting) {
    if (selecting) {
        browser_.set_selecting(true);
        if (browser_.selecting()) build_toolbar();
        return;
    }
    if (toolbar_) lv_obj_delete(toolbar_);
    toolbar_ = nullptr;
    browser_.set_selecting(false);
}

void FileBrowserPage::update_selection() {
    const std::size_t count = browser_.selected_count();
    selection_title_ = count == 0 ? "Select Items" : std::to_string(count) + " Selected";
    if (toolbar_) {
        lv_obj_set_state(move_button_, LV_STATE_DISABLED, count == 0);
        lv_obj_set_state(copy_button_, LV_STATE_DISABLED, count == 0);
        lv_obj_set_state(rename_button_, LV_STATE_DISABLED, count != 1);
        lv_obj_set_state(delete_button_, LV_STATE_DISABLED, count == 0);
    }
    if (home_) home_->refresh_navigation();
}

void FileBrowserPage::build_toolbar() {
    if (toolbar_ || !contents_) return;
    toolbar_ = lv_container_create(contents_, LV_FLEX_FLOW_ROW);
    lv_obj_set_size(toolbar_, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(toolbar_, kToolbarPadding, 0);
    lv_obj_set_style_pad_column(toolbar_, kToolbarPadding, 0);
    lv_obj_set_style_border_color(toolbar_, lv_color_hex(0xc0c0c0), 0);
    lv_obj_set_style_border_width(toolbar_, 1, 0);
    lv_obj_set_style_border_side(toolbar_, LV_BORDER_SIDE_TOP, 0);

    auto add = [this](const char *text, lv_button_style_t style) {
        auto button = lv_button_create(toolbar_, style);
        lv_obj_set_size(button, 0, kToolbarButtonHeight);
        lv_obj_set_flex_grow(button, 1);
        lv_obj_set_style_opa(button, LV_OPA_40, LV_STATE_DISABLED);
        lv_button_set_text(button, text);
        return button;
    };
    move_button_ = add("Move", LV_BUTTON_STYLE_SECONDARY);
    copy_button_ = add("Copy", LV_BUTTON_STYLE_SECONDARY);
    rename_button_ = add("Rename", LV_BUTTON_STYLE_SECONDARY);
    delete_button_ = add("Delete", LV_BUTTON_STYLE_DESTRUCTIVE);
    lv_obj_add_event_fn(move_button_, LV_EVENT_CLICKED, [this](lv_event_t *) { open_move(); });
    lv_obj_add_event_fn(rename_button_, LV_EVENT_CLICKED, [this](lv_event_t *) { open_rename(); });
    lv_obj_add_event_fn(delete_button_, LV_EVENT_CLICKED, [this](lv_event_t *) { confirm_delete(); });
    update_selection();
}

void FileBrowserPage::reload_directory() {
    if (toolbar_) lv_obj_delete(toolbar_);
    toolbar_ = nullptr;
    file_browser_invalidate();
    browser_.reload();
    if (home_) home_->refresh_navigation();
}

void FileBrowserPage::show_error(const char *title, const std::string &path, int error) {
    if (!contents_) return;
    const std::string message = name_of(path) + "\n" + strerror(error);
    auto modal = lv_modal_open(lv_obj_get_screen(contents_));
    lv_modal_title_create(modal, title);
    lv_modal_message_create(modal, message.c_str());
    lv_modal_button_create(modal, "Close", LV_MODAL_BUTTON_TYPE_PRIMARY,
                           [modal](lv_event_t *) { lv_modal_close(modal); });
}

void FileBrowserPage::open_rename() {
    if (browser_.selected_count() != 1 || rename_modal_) return;
    const std::string path = browser_.selected_paths().front();
    const std::string name = name_of(path);
    std::size_t stem = name.size();
    if (!is_directory(path)) {
        const std::size_t dot = name.rfind('.');
        if (dot != std::string::npos && dot > 0) stem = dot;
    }

    auto screen = lv_obj_get_screen(contents_);
    rename_modal_ = lv_modal_open(screen);
    lv_modal_title_create(rename_modal_, "Rename");

    auto textarea = lv_textarea_create(rename_modal_);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_text(textarea, name.c_str());
    lv_textarea_set_cursor_pos(textarea, (int32_t)utf8_length(name.substr(0, stem)));
    lv_obj_set_width(textarea, LV_PCT(100));
    lv_obj_set_font_role(textarea, LV_WIDGETS_FONT_BODY);

    rename_error_ = lv_modal_message_create(rename_modal_, "");
    lv_obj_set_style_text_color(rename_error_, lv_color_hex(0xd32f2f), 0);
    lv_obj_set_hidden(rename_error_, true);

    auto buttons = lv_container_create(rename_modal_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, kModalPadding, 0);

    /* Deleting the keyboard inside its own event hangs LVGL, so closing goes through post(). */
    auto cancel = [this](lv_event_t *) { post([](FileBrowserPage &page) { page.close_rename(); }); };
    auto submit = [this, textarea](lv_event_t *) {
        std::string text = lv_textarea_get_text(textarea);
        post([text](FileBrowserPage &page) { page.submit_rename(text); });
    };
    lv_modal_button_create(buttons, "Cancel", LV_MODAL_BUTTON_TYPE_SECONDARY, cancel);
    lv_modal_button_create(buttons, "Rename", LV_MODAL_BUTTON_TYPE_PRIMARY, submit);

    keyboard_ = lv_keyboard_create(screen);
    lv_obj_set_ignore_layout(keyboard_, true);
    lv_obj_set_size(keyboard_, LV_PCT(100), LV_PCT(40));
    lv_obj_set_font_role(keyboard_, LV_WIDGETS_FONT_BODY);
    lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard_, textarea);
    lv_obj_add_event_fn(keyboard_, LV_EVENT_READY, submit);
    lv_obj_add_event_fn(keyboard_, LV_EVENT_CANCEL, cancel);

    lv_obj_update_layout(screen);
    const int32_t above = lv_obj_get_height(screen) - lv_obj_get_height(keyboard_);
    lv_obj_align(rename_modal_, LV_ALIGN_TOP_MID, 0,
                 std::max<int32_t>(0, (above - lv_obj_get_height(rename_modal_)) / 2));
}

void FileBrowserPage::close_rename() {
    if (keyboard_) lv_obj_delete(keyboard_);
    if (rename_modal_) lv_modal_close(rename_modal_);
    keyboard_ = rename_modal_ = rename_error_ = nullptr;
}

void FileBrowserPage::submit_rename(const std::string &name) {
    if (!rename_modal_) return;
    const std::string from = browser_.selected_paths().front();
    const std::string old_name = name_of(from);
    const std::string to = browser_.path() + "/" + name;

    std::string error;
    if (name.empty()) {
        error = "Enter a name.";
    } else if (name == "." || name == "..") {
        error = "This name is not allowed.";
    } else if (name.find_first_of(kInvalidNameChars) != std::string::npos) {
        error = "A name can't contain / \\ : * ? \" < > |";
    } else if (name == old_name) {
        close_rename();
        return;
    } else if (strcasecmp(name.c_str(), old_name.c_str()) != 0 && exists(to)) {
        error = "\"" + name + "\" already exists.";
    } else if (rename(from.c_str(), to.c_str()) != 0) {
        error = strerror(errno);
        ESP_LOGE(TAG, "rename %s -> %s: %s", from.c_str(), to.c_str(), error.c_str());
    }
    if (!error.empty()) {
        lv_label_set_text(rename_error_, error.c_str());
        lv_obj_set_hidden(rename_error_, false);
        return;
    }

    close_rename();
    media_cache_forget(from);
    reload_directory();
}

void FileBrowserPage::confirm_delete() {
    const std::size_t count = browser_.selected_count();
    if (count == 0) return;
    const std::string target = count == 1 ? "\"" + name_of(browser_.selected_paths().front()) + "\""
                                          : std::to_string(count) + " items";
    const std::string message =
        "Are you sure you want to delete " + target + "?\nThis can't be undone.";
    auto modal = lv_modal_open(lv_obj_get_screen(contents_));
    lv_modal_title_create(modal, "Delete Permanently");
    lv_modal_message_create(modal, message.c_str());
    auto buttons = lv_container_create(modal, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, kModalPadding, 0);
    lv_modal_button_create(buttons, "Cancel", LV_MODAL_BUTTON_TYPE_SECONDARY,
                           [modal](lv_event_t *) { lv_modal_close(modal); });
    lv_modal_button_create(buttons, "Delete", LV_MODAL_BUTTON_TYPE_DESTRUCTIVE,
                           [this, modal](lv_event_t *) {
                               lv_modal_close(modal);
                               start_delete();
                           });
}

void FileBrowserPage::start_delete() {
    std::vector<std::string> paths = browser_.selected_paths();
    if (!file_operation_delete(paths)) {
        auto modal = lv_modal_open(lv_obj_get_screen(contents_));
        lv_modal_title_create(modal, "Cannot delete");
        lv_modal_message_create(modal, "Another file operation is running");
        lv_modal_button_create(modal, "Close", LV_MODAL_BUTTON_TYPE_PRIMARY,
                               [modal](lv_event_t *) { lv_modal_close(modal); });
        return;
    }
    browser_.on_disappear();
    operation_paths_ = std::move(paths);
    open_progress("Deleting");
}

void FileBrowserPage::open_progress(const char *title) {
    progress_modal_ = lv_modal_open(lv_obj_get_screen(contents_));
    lv_modal_title_create(progress_modal_, title);
    progress_label_ = lv_modal_message_create(progress_modal_, "");
    lv_label_set_long_mode(progress_label_, LV_LABEL_LONG_MODE_DOTS);

    progress_bar_ = lv_bar_create(progress_modal_);
    lv_obj_set_size(progress_bar_, LV_PCT(100), 12);
    lv_obj_set_style_radius(progress_bar_, 6, 0);
    lv_obj_set_style_radius(progress_bar_, 6, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(progress_bar_, lv_color_hex(0xe0e0e0), 0);
    lv_obj_set_style_bg_opa(progress_bar_, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(progress_bar_, lv_color_hex(0x2196f3), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(progress_bar_, LV_OPA_COVER, LV_PART_INDICATOR);

    lv_modal_button_create(progress_modal_, "Cancel", LV_MODAL_BUTTON_TYPE_SECONDARY,
                           [](lv_event_t *e) {
                               file_operation_cancel();
                               lv_obj_set_state(lv_event_get_target_obj(e), LV_STATE_DISABLED, true);
                           });
    progress_timer_ = lv_timer_create([](lv_timer_t *timer) {
        static_cast<FileBrowserPage *>(lv_timer_get_user_data(timer))->poll_operation();
    }, kProgressPeriodMs, this);
    poll_operation();
}

void FileBrowserPage::close_progress() {
    if (progress_timer_) lv_timer_delete(progress_timer_);
    if (progress_modal_) lv_modal_close(progress_modal_);
    progress_timer_ = nullptr;
    progress_modal_ = progress_label_ = progress_bar_ = nullptr;
}

void FileBrowserPage::poll_operation() {
    const FileOperationStatus status = file_operation_status();
    if (status.finished) {
        finish_operation(status, "Delete failed");
        return;
    }
    const std::size_t step = std::min(status.done + 1, status.total);
    const std::string text = std::to_string(step) + " / " + std::to_string(status.total) + "  " +
                             status.current;
    lv_label_set_text(progress_label_, text.c_str());
    lv_bar_set_range(progress_bar_, 0, (int32_t)status.total);
    lv_bar_set_value(progress_bar_, (int32_t)status.done, LV_ANIM_OFF);
}

void FileBrowserPage::finish_operation(const FileOperationStatus &status, const char *failure) {
    close_progress();
    for (const std::string &path : operation_paths_) media_cache_forget(path);
    operation_paths_.clear();
    reload_directory();
    if (status.error != 0) show_error(failure, status.failed_path, status.error);
}

void FileBrowserPage::open_move() {
    if (browser_.selected_count() == 0) return;
    std::vector<std::string> sources = browser_.selected_paths();
    FilePickerOptions options;
    options.start = browser_.path();
    options.root = browser_.root();
    options.confirm = "Move Here";
    options.excluded = sources;
    options.can_pick = [source = browser_.path()](const std::string &path) { return path != source; };
    options.on_pick = [weak = weak_from_this(), sources](const std::string &destination) {
        if (auto page = weak.lock()) page->start_move(sources, destination);
    };
    screen_manager.push(std::make_shared<FilePickerScreen>(std::move(options)));
}

void FileBrowserPage::start_move(const std::vector<std::string> &sources,
                                 const std::string &destination) {
    std::vector<std::string> existing;
    for (const std::string &source : sources) {
        const std::string name = name_of(source);
        if (exists(destination + "/" + name)) existing.push_back(name);
    }
    if (existing.empty()) {
        run_move(sources, destination, false);
        return;
    }

    const std::string place = file_browser_title(destination);
    const std::string message =
        existing.size() == 1
            ? "An item named \"" + existing.front() + "\" already exists in \"" + place + "\"."
            : std::to_string(existing.size()) + " items with the same names already exist in \"" +
                  place + "\".";
    auto modal = lv_modal_open(lv_obj_get_screen(contents_));
    lv_modal_title_create(modal, existing.size() == 1 ? "Item Already Exists" : "Items Already Exist");
    lv_modal_message_create(modal, message.c_str());
    auto buttons = lv_container_create(modal, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, kModalPadding, 0);
    lv_modal_button_create(buttons, "Cancel", LV_MODAL_BUTTON_TYPE_SECONDARY,
                           [modal](lv_event_t *) { lv_modal_close(modal); });
    lv_modal_button_create(buttons, "Skip", LV_MODAL_BUTTON_TYPE_SECONDARY,
                           [this, modal, sources, destination](lv_event_t *) {
                               lv_modal_close(modal);
                               run_move(sources, destination, true);
                           });
    lv_modal_button_create(buttons, "Keep Both", LV_MODAL_BUTTON_TYPE_PRIMARY,
                           [this, modal, sources, destination](lv_event_t *) {
                               lv_modal_close(modal);
                               run_move(sources, destination, false);
                           });
}

void FileBrowserPage::run_move(const std::vector<std::string> &sources,
                               const std::string &destination, bool skip_existing) {
    std::string failed;
    int error = 0;
    for (const std::string &source : sources) {
        std::string name = name_of(source);
        if (exists(destination + "/" + name)) {
            if (skip_existing) continue;
            name = unused_name(destination, name, is_directory(source));
        }
        const std::string target = destination + "/" + name;
        if (rename(source.c_str(), target.c_str()) != 0) {
            failed = source;
            error = errno;
            ESP_LOGE(TAG, "rename %s -> %s: %s", source.c_str(), target.c_str(), strerror(error));
            break;
        }
        media_cache_forget(source);
    }
    reload_directory();
    if (error != 0) show_error("Move failed", failed, error);
}

std::shared_ptr<Playlist> FileBrowserPage::make_playlist(std::size_t index) const {
    const PsramVector<DirectoryEntry> &entries = browser_.entries();
    const MediaKind kind = entries[index].kind;
    std::vector<PlaylistItem> items;
    std::size_t current = 0;
    for (std::size_t i = 0; i < entries.size(); i++) {
        const DirectoryEntry &entry = entries[i];
        if (entry.directory || entry.kind != kind) continue;
        if (i == index) current = items.size();
        items.push_back({ entry.name.c_str(), browser_.entry_path(i) });
    }
    return std::make_shared<Playlist>(std::move(items), current);
}

void FileBrowserPage::did_open_file(std::size_t index) {
    const DirectoryEntry &entry = browser_.entries()[index];
    if (entry.kind == MediaKind::Audio) {
        media_cache_resolve(browser_.entry_path(index), MetaWantInfo, {}, kResolveTimeoutMs);
        screen_manager.push(std::make_shared<AudioPlayerScreen>(make_playlist(index)));
    } else if (entry.kind == MediaKind::Video) {
        screen_manager.push(std::make_shared<VideoPlayerScreen>(make_playlist(index)));
    } else if (entry.kind == MediaKind::Image) {
        screen_manager.push(std::make_shared<ImageViewerScreen>(make_playlist(index)));
    }
}

void FileBrowserPage::did_change_directory() {
    if (home_) home_->refresh_navigation();
}

void FileBrowserPage::did_change_selection() {
    update_selection();
}

void FileBrowserPage::build(lv_obj_t *contents) {
    contents_ = contents;
    toolbar_ = nullptr;
    lv_obj_set_style_bg_color(contents, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(contents, LV_OPA_COVER, 0);
    browser_.build(contents);
    if (browser_.selecting()) build_toolbar();
}

void FileBrowserPage::on_appear() {
    browser_.on_appear();
}

void FileBrowserPage::on_disappear() {
    browser_.on_disappear();
}

void FileBrowserPage::save_state() {
    browser_.save_state();
    contents_ = nullptr;
    toolbar_ = nullptr;
}

bool FileBrowserPage::is_under(const std::string &mount_point) const {
    return path_is_under(browser_.root(), mount_point);
}
