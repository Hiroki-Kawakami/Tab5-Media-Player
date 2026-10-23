/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "file_browser.hpp"
#include "bsp.h"
#include "media/media_cache.hpp"
#include "media_player.hpp"
#include "resources.h"
#include "screens/image_object.hpp"
#include "usb_host_msc.hpp"

#include <algorithm>
#include <cstring>
#include <sys/stat.h>

static constexpr int32_t kRowHeight = 80;
static constexpr int32_t kThumbSide = 56;
static constexpr ImageSize kThumbBox = { kThumbSide, kThumbSide };
static constexpr int32_t kListRowHeight = 132;
static constexpr ImageSize kListThumbBox = { 192, 108 };
static constexpr uint32_t kListThumbColor = 0xeeeeee;
static constexpr ImageSize kGridThumbBox = { 200, 150 };
static constexpr int32_t kGridMinGap = 12;
static constexpr int32_t kGridNameGap = 8;
static constexpr int32_t kGridRowGap = 24;
static constexpr int32_t kGridHighlightPad = 8;
static constexpr std::size_t kLookahead = 2;
static constexpr std::size_t kPrefetchLimit = 24;
static constexpr uint32_t kSelectedColor = 0xd6eafd;
static constexpr lv_opa_t kEnabledOpa = LV_OPA_COVER;
static constexpr lv_opa_t kDisabledOpa = LV_OPA_40;

static constexpr const char *kStorages[] = { kSdMountPoint, kUsbMountPoint };

static FileBrowser *s_visible;
static uint32_t s_generation = 1;

void file_browser_invalidate() {
    s_generation++;
}

static bool has_tags(MediaKind kind) {
    return kind == MediaKind::Audio || kind == MediaKind::Video;
}

static bool has_thumbnail(MediaKind kind) {
    return has_tags(kind) || kind == MediaKind::Image;
}

static int32_t body_line_height() {
    return lv_font_get_line_height(lv_widgets_resolved_font(LV_WIDGETS_FONT_BODY));
}

static void drop_thumbnail(lv_obj_t *frame) {
    lv_obj_t *first = lv_obj_get_child(frame, 0);
    if (lv_obj_check_type(first, &lv_image_class)) lv_obj_delete(first);
}

static bool is_usb(const char *mount_point) {
    return strcmp(mount_point, kUsbMountPoint) == 0;
}

static const char *storage_name(const char *mount_point) {
    return is_usb(mount_point) ? "USB Drive" : "SD Card";
}

static bool storage_mounted(const char *mount_point) {
    return is_usb(mount_point) ? usb_host::mounted(mount_point) : bsp_sd_is_mounted();
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

std::string file_browser_title(const std::string &path) {
    if (path.empty()) return "Storage";
    for (const char *mount_point : kStorages) {
        if (path == mount_point) return storage_name(mount_point);
    }
    return path.substr(path.rfind('/') + 1);
}

FileBrowser::~FileBrowser() {
    release_requests();
}

void FileBrowser::open(const std::string &path, const std::string &root) {
    std::string start = path;
    struct stat st;
    while (!start.empty() && (!mount_point_of(start) || stat(start.c_str(), &st) != 0 ||
                              !S_ISDIR(st.st_mode) ||
                              (!root.empty() && !path_is_under(start, root)))) {
        start = parent_of(start);
    }

    levels_.clear();
    std::string base = root;
    if (root.empty()) {
        levels_.push_back({});
        levels_.back().title = file_browser_title({});
        if (start.empty()) return;
        base = mount_point_of(start);
    } else if (start.empty()) {
        start = root;
    }
    std::string current = base;
    while (true) {
        levels_.push_back({});
        levels_.back().path = current;
        levels_.back().title = file_browser_title(current);
        if (current.size() >= start.size()) break;
        const std::size_t end = start.find('/', current.size() + 1);
        current = start.substr(0, end);
    }
}

void FileBrowser::defer(std::function<void(FileBrowser &)> fn) {
    lv_async_call([this, weak = std::weak_ptr<bool>(alive_), fn = std::move(fn)] {
        if (!weak.expired()) fn(*this);
    });
}

void FileBrowser::list(Level &level) {
    level.generation = s_generation;
    level.entries.clear();
    level.storages.clear();
    if (!level.path.empty()) {
        level.opened = media_directory_list(level.path, &level.entries);
        return;
    }
    level.opened = true;
    for (const char *mount_point : kStorages) {
        if (is_usb(mount_point) && !media_player_usb_connected()) continue;
        level.storages.push_back(mount_point);
        level.entries.push_back({ PsramString(storage_name(mount_point)), true, MediaKind::None });
    }
}

void FileBrowser::build(lv_obj_t *contents) {
    lv_obj_set_scrollable(contents, false);
    layout_ = settings_browser_layout();
    contents_ = contents;
    body_ = nullptr;
    if (level().generation != s_generation) list(level());
    build_view();
}

void FileBrowser::build_view() {
    if (body_) lv_obj_delete(body_);
    body_ = list_ = nullptr;
    const Level &current = level();
    if (!current.opened || current.entries.empty()) {
        lv_obj_set_flex_align(contents_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
        body_ = lv_label_create(contents_);
        lv_obj_move_to_index(body_, 0);
        lv_label_set_text(body_, current.opened ? "No files" : "Cannot open directory");
        lv_obj_set_font_role(body_, LV_WIDGETS_FONT_BODY);
        lv_obj_set_style_text_color(body_, lv_color_hex(0x808080), 0);
        return;
    }
    lv_obj_set_flex_align(contents_, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    list_ = layout_ == BrowserLayout::Grid ? lv_grid_create(contents_, this)
                                           : lv_list_create(contents_, this);
    body_ = list_;
    lv_obj_move_to_index(list_, 0);
    lv_obj_add_event_fn(list_, LV_EVENT_SCROLL, [this](lv_event_t *) { request_visible(); });
    lv_obj_update_layout(contents_);
    if (current.scroll_y > 0) lv_obj_scroll_to_y(list_, current.scroll_y, LV_ANIM_OFF);
    observe();
}

void FileBrowser::observe() {
    if (!list_) return;
    s_visible = this;
    requested_first_ = SIZE_MAX;
    prefetched_ = false;
    if (!token_) {
        token_ = media_cache_token();
        idle_token_ = media_cache_token();
    }
    media_cache_observe(token_, meta_ready);
    request_visible();
}

void FileBrowser::show_level() {
    if (level().generation != s_generation) list(level());
    if (contents_) {
        release_requests();
        if (list_ && !level().entries.empty()) {
            if (layout_ == BrowserLayout::Grid) {
                lv_grid_reload_data(list_, false);
            } else {
                lv_list_reload_data(list_, false);
            }
            lv_obj_scroll_to_y(list_, level().scroll_y, LV_ANIM_OFF);
            observe();
        } else {
            build_view();
        }
    }
    if (delegate_) delegate_->did_change_directory();
}

void FileBrowser::push(std::string path) {
    if (list_) level().scroll_y = lv_obj_get_scroll_y(list_);
    levels_.push_back({});
    levels_.back().title = file_browser_title(path);
    levels_.back().path = std::move(path);
    show_level();
}

void FileBrowser::go_back() {
    if (!can_go_back()) return;
    levels_.pop_back();
    show_level();
}

void FileBrowser::go_to_root() {
    if (levels_.size() == 1) return;
    levels_.resize(1);
    show_level();
}

bool FileBrowser::eject(const std::string &mount_point) {
    auto under = [&mount_point](const Level &level) {
        return !level.path.empty() && path_is_under(level.path, mount_point);
    };
    auto first = std::find_if(levels_.begin(), levels_.end(), under);
    if (first == levels_.end()) return true;
    levels_.erase(first, levels_.end());
    if (levels_.empty()) return false;
    show_level();
    return true;
}

void FileBrowser::refresh_storages() {
    for (Level &level : levels_) {
        if (level.path.empty()) level.generation = 0;
    }
    if (level().path.empty()) show_level();
}

void FileBrowser::reload() {
    selecting_ = false;
    selected_.clear();
    selected_count_ = 0;
    list(level());
    if (!contents_) return;
    release_requests();
    if (list_ && !level().entries.empty()) {
        reload_entries();
        observe();
    } else {
        level().scroll_y = 0;
        build_view();
    }
}

void FileBrowser::set_layout(BrowserLayout layout) {
    if (layout == settings_browser_layout()) return;
    settings_set_browser_layout(layout);
    settings_commit();
    if (!list_) return;

    const std::size_t first = first_visible_entry();
    release_requests();
    layout_ = layout;
    level().scroll_y = 0;
    build_view();
    if (list_ && first != SIZE_MAX) show_entry(first);
}

void FileBrowser::save_state() {
    release_requests();
    if (list_) level().scroll_y = lv_obj_get_scroll_y(list_);
    list_ = body_ = contents_ = nullptr;
}

void FileBrowser::on_appear() {
    observe();
}

void FileBrowser::on_disappear() {
    release_requests();
}

void FileBrowser::open_storage(const char *mount_point) {
    const bool usb = is_usb(mount_point);
    const esp_err_t err = usb ? media_player_mount_usb() : media_player_mount_sd();
    if (err == ESP_OK) {
        defer([path = std::string(mount_point)](FileBrowser &browser) { browser.push(path); });
        return;
    }
    std::string text = usb && err == ESP_ERR_NOT_FOUND ? "No USB drive connected"
                     : usb                              ? "Failed to mount USB drive"
                                                        : "Failed to mount SD card";
    if (err != ESP_ERR_NOT_FOUND) text = text + "\n" + esp_err_to_name(err);
    auto modal = lv_modal_open(lv_obj_get_screen(contents_));
    lv_modal_title_create(modal, storage_name(mount_point));
    lv_modal_message_create(modal, text.c_str());
    lv_modal_button_create(modal, "Close", LV_MODAL_BUTTON_TYPE_PRIMARY,
                           [modal](lv_event_t *) { lv_modal_close(modal); });
}

std::string FileBrowser::entry_path(std::size_t index) const {
    if (level().path.empty()) return level().storages[index];
    return level().path + "/" + level().entries[index].name.c_str();
}

void FileBrowser::set_selecting(bool selecting) {
    if (selecting && !list_) return;
    selecting_ = selecting;
    selected_.assign(selecting ? entries().size() : 0, false);
    selected_count_ = 0;
    reload_entries();
    if (delegate_) delegate_->did_change_selection();
}

void FileBrowser::toggle_selected(std::size_t index) {
    selected_[index] = !selected_[index];
    if (selected_[index]) {
        selected_count_++;
    } else {
        selected_count_--;
    }
    rebind_entry(index);
    if (delegate_) delegate_->did_change_selection();
}

void FileBrowser::select_all(bool selected) {
    selected_.assign(entries().size(), selected);
    selected_count_ = selected ? entries().size() : 0;
    reload_entries();
    if (delegate_) delegate_->did_change_selection();
}

std::vector<std::string> FileBrowser::selected_paths() const {
    std::vector<std::string> paths;
    for (std::size_t i = 0; i < selected_.size(); i++) {
        if (selected_[i]) paths.push_back(entry_path(i));
    }
    return paths;
}

void FileBrowser::release_requests() {
    if (s_visible == this) s_visible = nullptr;
    visible_.clear();
    if (!token_) return;
    media_cache_unobserve(token_);
    media_cache_cancel(token_);
    media_cache_cancel(idle_token_);
}

bool FileBrowser::keep_visible(const char *path, void *ctx) {
    auto *browser = static_cast<FileBrowser *>(ctx);
    for (const PsramString &kept : browser->visible_) {
        if (strcmp(kept.c_str(), path) == 0) return true;
    }
    return false;
}

void FileBrowser::request_visible() {
    const PsramVector<DirectoryEntry> &items = entries();
    if (!list_ || items.empty() || level().path.empty()) return;

    std::size_t first = first_visible_entry();
    if (first == SIZE_MAX) first = 0;
    if (first == requested_first_) return;
    requested_first_ = first;
    const bool grid = layout_ == BrowserLayout::Grid;
    const std::size_t columns = grid ? lv_grid_column_count(list_) : 1;
    const int32_t row_height = grid ? cellHeight() + rowSpacing() : rowHeight();
    const int32_t height = lv_obj_get_height(list_);
    const std::size_t rows = height > 0 ? (std::size_t)(height / row_height) + 1 : 1;
    const std::size_t first_row = first / columns;
    const std::size_t start = (first_row > kLookahead ? first_row - kLookahead : 0) * columns;
    const std::size_t end = std::min(items.size(), (first_row + rows + kLookahead) * columns);

    /* What is left of the range the last scroll asked for is kept rather than
       cancelled and asked for again: a thumbnail that is still on screen would
       otherwise restart from the card on every row the finger travels, and one
       that has scrolled off is withdrawn even if it is already being read. */
    visible_.clear();
    for (std::size_t i = start; i < end; i++) visible_.push_back(PsramString(entry_path(i).c_str()));
    media_cache_retain(token_, keep_visible, this);

    /* Titles first for the whole visible range, then the thumbnails: reading a
       tag without its cover art is a fraction of the cost, so the rows fill in
       with text while the pictures are still coming. An image file has no
       title to show, so it only takes part in the second pass. */
    for (std::size_t i = start; i < end; i++) {
        if (items[i].directory || !has_tags(items[i].kind)) continue;
        media_cache_request(entry_path(i), MetaWantInfo, {}, MetaPriority::Visible, token_);
    }
    for (std::size_t i = start; i < end; i++) {
        if (items[i].directory || !has_thumbnail(items[i].kind)) continue;
        media_cache_request(entry_path(i), MetaWantInfo | MetaWantThumbnail, thumb_box(),
                            MetaPriority::Visible, token_);
    }
    prefetch_rest();
}

void FileBrowser::prefetch_rest() {
    if (prefetched_) return;
    prefetched_ = true;
    const PsramVector<DirectoryEntry> &items = entries();
    std::size_t queued = 0;
    for (std::size_t i = 0; i < items.size() && queued < kPrefetchLimit; i++) {
        if (items[i].directory || items[i].kind != MediaKind::Audio) continue;
        media_cache_request(entry_path(i), MetaWantInfo | MetaWantThumbnail, thumb_box(),
                            MetaPriority::Idle, idle_token_);
        queued++;
    }
}

void FileBrowser::meta_ready(const std::string &path) {
    FileBrowser *browser = s_visible;
    if (!browser || !browser->list_ || browser->path().empty()) return;
    if (path.compare(0, browser->path().size(), browser->path()) != 0) return;

    for (std::size_t i = 0; i < browser->entries().size(); i++) {
        if (browser->entry_path(i) != path) continue;
        browser->rebind_entry(i);
        return;
    }
}

void FileBrowser::rebind_entry(std::size_t index) {
    if (layout_ == BrowserLayout::Grid) {
        lv_grid_reload_cell(list_, index);
    } else if (lv_obj_t *row = lv_list_row_for_index(list_, index)) {
        bindRow(row, index);
    }
}

void FileBrowser::reload_entries() {
    if (!list_) return;
    if (layout_ == BrowserLayout::Grid) {
        lv_grid_reload_data(list_, true);
    } else {
        lv_list_reload_data(list_, true);
    }
}

std::size_t FileBrowser::first_visible_entry() const {
    return layout_ == BrowserLayout::Grid ? lv_grid_first_visible_cell(list_)
                                          : lv_list_first_visible_row(list_);
}

void FileBrowser::show_entry(std::size_t index) {
    if (layout_ == BrowserLayout::Grid) {
        lv_grid_show_cell(list_, index);
    } else {
        lv_list_show_row(list_, index);
    }
}

ImageSize FileBrowser::thumb_box() const {
    switch (layout_) {
    case BrowserLayout::List: return kListThumbBox;
    case BrowserLayout::Grid: return kGridThumbBox;
    default: return kThumbBox;
    }
}

bool FileBrowser::entry_enabled(std::size_t index) const {
    if (selecting_ || !delegate_) return true;
    return delegate_->is_enabled(entry_path(index), entries()[index]);
}

bool FileBrowser::entry_clickable(std::size_t index) const {
    if (selecting_) return true;
    const DirectoryEntry &entry = entries()[index];
    return entry_enabled(index) && (entry.directory || entry.kind != MediaKind::None);
}

std::size_t FileBrowser::rowCount() const {
    return entries().size();
}

int32_t FileBrowser::rowHeight() const {
    return layout_ == BrowserLayout::List ? kListRowHeight : kRowHeight;
}

lv_obj_t *FileBrowser::createRow(lv_obj_t *parent) {
    const bool list = layout_ == BrowserLayout::List;
    lv_obj_t *row = lv_list_row_create(parent);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 24, 0);

    lv_obj_t *frame = row;
    if (list) {
        frame = lv_container_create(row, lv_color_hex(kListThumbColor));
        lv_obj_set_size(frame, kListThumbBox.width, kListThumbBox.height);
        lv_obj_set_style_radius(frame, 6, 0);
        lv_obj_set_style_clip_corner(frame, true, 0);
        lv_obj_set_clickable(frame, false);
    }
    lv_obj_t *icon = lv_label_create(frame);
    lv_obj_set_width(icon, kThumbSide);
    lv_obj_set_style_text_align(icon, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(icon, list ? &icon_48 : &icon_36, 0);
    if (list) lv_obj_center(icon);

    const int32_t line_height = body_line_height();
    lv_obj_t *name = lv_label_create(row);
    lv_obj_set_flex_grow(name, 1);
    if (list) {
        lv_obj_set_height(name, LV_SIZE_CONTENT);
        lv_obj_set_style_max_height(name, 2 * line_height, 0);
    } else {
        lv_obj_set_height(name, line_height);
    }
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
    lv_obj_set_style_bg_color(row, lv_color_hex(kSelectedColor), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_CHECKED);
    lv_obj_t *separator = lv_hor_separator_create(row);
    lv_obj_set_ignore_layout(separator, true);
    lv_obj_set_clickable(separator, false);
    lv_obj_align(separator, LV_ALIGN_BOTTOM_MID, 0, 0);
    return row;
}

void FileBrowser::bindRow(lv_obj_t *row, std::size_t index) {
    const DirectoryEntry &entry = entries()[index];
    const bool enabled = entry_enabled(index);
    const lv_opa_t opa = enabled ? kEnabledOpa : kDisabledOpa;
    lv_obj_t *frame = layout_ == BrowserLayout::List ? lv_obj_get_child(row, 0) : row;
    drop_thumbnail(frame);
    lv_obj_t *name = lv_obj_get_child(row, 1);
    lv_obj_t *arrow = lv_obj_get_child(row, 2);
    lv_label_set_text(name, entry.name.c_str());
    lv_obj_set_style_text_opa(name, opa, 0);
    lv_obj_set_hidden(arrow, !entry.directory);
    lv_obj_set_style_text_opa(arrow, opa, 0);
    if (frame != row) lv_obj_set_style_bg_opa(frame, opa, 0);
    lv_obj_set_clickable(row, entry_clickable(index));
    lv_obj_set_state(row, LV_STATE_CHECKED, selecting_ && selected_[index]);
    bind_thumbnail(frame, index, enabled);
}

void FileBrowser::didSelectRow(std::size_t index) {
    if (selecting_) {
        toggle_selected(index);
        return;
    }
    if (!entry_enabled(index)) return;
    if (path().empty()) {
        open_storage(level().storages[index]);
    } else if (entries()[index].directory) {
        defer([path = entry_path(index)](FileBrowser &browser) { browser.push(path); });
    } else if (delegate_) {
        delegate_->did_open_file(index);
    }
}

std::size_t FileBrowser::cellCount() const {
    return entries().size();
}

int32_t FileBrowser::cellWidth() const {
    return kGridThumbBox.width;
}

int32_t FileBrowser::cellHeight() const {
    return kGridThumbBox.height + kGridNameGap + 2 * body_line_height();
}

int32_t FileBrowser::columnSpacing() const {
    return kGridMinGap;
}

int32_t FileBrowser::rowSpacing() const {
    return kGridRowGap;
}

lv_obj_t *FileBrowser::createCell(lv_obj_t *parent) {
    lv_obj_t *cell = lv_container_create(parent);
    lv_obj_set_scrollable(cell, false);
    /* OVERFLOW_VISIBLE only lets children past the cell by its ext draw size. */
    lv_obj_set_overflow_visible(cell, true);
    lv_obj_add_event_fn(cell, LV_EVENT_REFR_EXT_DRAW_SIZE, [](lv_event_t *e) {
        lv_event_set_ext_draw_size(e, kGridHighlightPad);
    });
    lv_obj_refresh_ext_draw_size(cell);
    lv_obj_set_state_trickle(cell, true);

    lv_obj_t *body = lv_container_create(cell);
    lv_obj_set_clickable(body, false);
    lv_obj_set_scrollable(body, false);
    lv_obj_set_pos(body, -kGridHighlightPad, -kGridHighlightPad);
    lv_obj_set_size(body, kGridThumbBox.width + 2 * kGridHighlightPad, LV_SIZE_CONTENT);
    lv_obj_set_style_pad_all(body, kGridHighlightPad, 0);
    lv_obj_set_flex_flow(body, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(body, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(body, kGridNameGap, 0);
    lv_obj_set_style_radius(body, 6 + kGridHighlightPad, 0);
    lv_obj_set_style_bg_color(body, lv_color_black(), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(body, 35, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(body, lv_color_hex(kSelectedColor), LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(body, LV_OPA_COVER, LV_STATE_CHECKED);

    lv_obj_t *frame = lv_container_create(body, lv_color_hex(kListThumbColor));
    lv_obj_set_size(frame, kGridThumbBox.width, kGridThumbBox.height);
    lv_obj_set_style_radius(frame, 6, 0);
    lv_obj_set_style_clip_corner(frame, true, 0);
    lv_obj_set_clickable(frame, false);

    lv_obj_t *icon = lv_label_create(frame);
    lv_obj_set_style_text_font(icon, &icon_48, 0);
    lv_obj_center(icon);

    lv_obj_t *name = lv_label_create(body);
    lv_obj_set_width(name, LV_PCT(100));
    lv_obj_set_style_max_height(name, 2 * body_line_height(), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_font_role(name, LV_WIDGETS_FONT_BODY);
    return cell;
}

void FileBrowser::bindCell(lv_obj_t *cell, std::size_t index) {
    const DirectoryEntry &entry = entries()[index];
    const bool enabled = entry_enabled(index);
    lv_obj_t *body = lv_obj_get_child(cell, 0);
    lv_obj_t *frame = lv_obj_get_child(body, 0);
    lv_obj_t *name = lv_obj_get_child(body, 1);
    drop_thumbnail(frame);
    lv_obj_set_clickable(cell, entry_clickable(index));
    lv_obj_set_state(cell, LV_STATE_CHECKED, selecting_ && selected_[index]);
    lv_label_set_text(name, entry.name.c_str());
    lv_obj_set_style_text_opa(name, enabled ? kEnabledOpa : kDisabledOpa, 0);
    bind_thumbnail(frame, index, enabled);
}

void FileBrowser::didSelectCell(std::size_t index) {
    didSelectRow(index);
}

void FileBrowser::bind_thumbnail(lv_obj_t *frame, std::size_t index, bool enabled) {
    const DirectoryEntry &entry = entries()[index];
    const char *icon = path().empty()                    ? (is_usb(level().storages[index])
                                                                ? TABLER_DEVICE_USB
                                                                : TABLER_DEVICE_SD_CARD)
                     : entry.directory                   ? TABLER_FOLDER_FILLED
                     : entry.kind == MediaKind::Video    ? TABLER_MOVIE
                     : entry.kind == MediaKind::Audio    ? TABLER_MUSIC
                     : entry.kind == MediaKind::Image    ? TABLER_PHOTO
                     : entry.kind == MediaKind::Playlist ? TABLER_PLAYLIST
                                                         : TABLER_FILE;
    const lv_opa_t opa = enabled ? kEnabledOpa : kDisabledOpa;
    lv_obj_t *label = lv_obj_get_child(frame, 0);
    lv_label_set_text(label, icon);
    lv_obj_set_style_text_opa(label, opa, 0);

    std::shared_ptr<const ImagePixels> pixels;
    if (!entry.directory && has_thumbnail(entry.kind)) {
        pixels = media_cache_thumbnail(entry_path(index), thumb_box());
    }
    lv_obj_set_hidden(label, pixels != nullptr);
    if (layout_ == BrowserLayout::Grid) {
        lv_obj_set_width(frame, pixels ? kGridThumbBox.width : kGridThumbBox.height);
        lv_obj_set_style_bg_opa(frame, pixels ? (lv_opa_t)LV_OPA_TRANSP : opa, 0);
    }
    /* Inserted first so flex puts it where the icon was; callers read the
       row's children by index before this point. */
    if (lv_obj_t *image = pixels ? image_object_create(frame, std::move(pixels)) : nullptr) {
        if (layout_ != BrowserLayout::Grid) {
            const ImageSize box = thumb_box();
            lv_obj_set_size(image, box.width, box.height);
            lv_image_set_inner_align(image, LV_IMAGE_ALIGN_CENTER);
        }
        if (layout_ != BrowserLayout::List) {
            lv_obj_set_style_radius(image, 6, 0);
            lv_obj_set_style_clip_corner(image, true, 0);
        }
        lv_obj_set_style_image_opa(image, opa, 0);
        lv_obj_move_to_index(image, 0);
    }
}
