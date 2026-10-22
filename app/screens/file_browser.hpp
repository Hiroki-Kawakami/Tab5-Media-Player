/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "media/image_codec.hpp"
#include "media/media_directory.hpp"
#include "settings.hpp"
#include "widgets.hpp"

class FileBrowserDelegate {
public:
    virtual ~FileBrowserDelegate() = default;
    virtual bool is_enabled(const std::string &, const DirectoryEntry &) const { return true; }
    virtual void did_open_file(std::size_t) {}
    virtual void did_change_directory() {}
    virtual void did_change_selection() {}
};

/* Every listing made before this is read again when it is next shown. */
void file_browser_invalidate();
/* What a directory is called in titles: the storage name for a mount point. */
std::string file_browser_title(const std::string &path);

/* One directory at a time in a container, with the levels above it kept with
   their listing and scroll position. An empty path is the list of storages. */
class FileBrowser : private ListDataSource, private GridDataSource {
public:
    explicit FileBrowser(FileBrowserDelegate *delegate) : delegate_(delegate) {}
    ~FileBrowser();

    /* Levels from root down to path; an empty root starts at the storages. */
    void open(const std::string &path, const std::string &root);
    void build(lv_obj_t *contents);
    void save_state();
    void on_appear();
    void on_disappear();

    const std::string &path() const { return level().path; }
    const std::string &root() const { return levels_.front().path; }
    const std::string &title() const { return level().title; }
    bool can_go_back() const { return levels_.size() > 1; }
    void go_back();
    void go_to_root();
    /* False when no level is left. */
    bool eject(const std::string &mount_point);
    void refresh_storages();
    void reload();
    void set_layout(BrowserLayout layout);

    const PsramVector<DirectoryEntry> &entries() const { return level().entries; }
    std::string entry_path(std::size_t index) const;

    bool selecting() const { return selecting_; }
    void set_selecting(bool selecting);
    void select_all(bool selected);
    std::size_t selected_count() const { return selected_count_; }
    std::vector<std::string> selected_paths() const;

private:
    struct Level {
        std::string path;
        std::string title;
        PsramVector<DirectoryEntry> entries;
        std::vector<const char *> storages;
        bool opened = false;
        uint32_t generation = 0;
        int32_t scroll_y = 0;
    };

    FileBrowserDelegate *delegate_;
    std::vector<Level> levels_;
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    BrowserLayout layout_ = BrowserLayout::Compact;
    lv_obj_t *view_ = nullptr;
    lv_obj_t *list_ = nullptr;
    std::size_t requested_first_ = SIZE_MAX;
    uint32_t token_ = 0;
    uint32_t idle_token_ = 0;
    bool prefetched_ = false;
    PsramVector<PsramString> visible_;
    bool selecting_ = false;
    PsramVector<bool> selected_;
    std::size_t selected_count_ = 0;

    Level &level() { return levels_.back(); }
    const Level &level() const { return levels_.back(); }
    void push(std::string path);
    void defer(std::function<void(FileBrowser &)> fn);
    void list(Level &level);
    void show_level();
    void build_view();
    void observe();
    void open_storage(const char *mount_point);
    void toggle_selected(std::size_t index);
    ImageSize thumb_box() const;
    std::size_t first_visible_entry() const;
    void show_entry(std::size_t index);
    void reload_entries();
    void rebind_entry(std::size_t index);
    void bind_thumbnail(lv_obj_t *frame, std::size_t index, bool enabled);
    bool entry_enabled(std::size_t index) const;
    bool entry_clickable(std::size_t index) const;
    void request_visible();
    void prefetch_rest();
    void release_requests();
    static void meta_ready(const std::string &path);
    static bool keep_visible(const char *path, void *ctx);

    std::size_t rowCount() const override;
    int32_t rowHeight() const override;
    lv_obj_t *createRow(lv_obj_t *parent) override;
    void bindRow(lv_obj_t *row, std::size_t index) override;
    void didSelectRow(std::size_t index) override;

    std::size_t cellCount() const override;
    int32_t cellWidth() const override;
    int32_t cellHeight() const override;
    int32_t columnSpacing() const override;
    int32_t rowSpacing() const override;
    lv_obj_t *createCell(lv_obj_t *parent) override;
    void bindCell(lv_obj_t *cell, std::size_t index) override;
    void didSelectCell(std::size_t index) override;
};
