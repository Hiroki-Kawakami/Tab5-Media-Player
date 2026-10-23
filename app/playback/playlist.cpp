/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "playlist.hpp"
#include "media/media_directory.hpp"
#include "media_player.hpp"
#include "esp_log.h"
#include "esp_timer.h"

#include <algorithm>
#include <cstdio>
#include <numeric>
#include <random>
#include <sys/stat.h>

static const char *TAG = "playlist";

static constexpr long kMaxPlaylistBytes = 512 * 1024;

static std::minstd_rand &random_engine() {
    static std::minstd_rand engine((uint32_t)esp_timer_get_time());
    return engine;
}

Playlist::Playlist(std::vector<PlaylistItem> items, std::size_t index) : items_(std::move(items)) {
    if (items_.empty()) items_.emplace_back();
    order_.resize(items_.size());
    std::iota(order_.begin(), order_.end(), 0);
    select(index);
}

bool Playlist::canStep(int delta, RepeatMode repeat) const {
    if (delta == 0) return false;
    if (repeat == RepeatMode::All) return true;
    const long long target = (long long)position_ + delta;
    return target >= 0 && target < (long long)items_.size();
}

bool Playlist::step(int delta, RepeatMode repeat) {
    if (!canStep(delta, repeat)) return false;
    const long long count = (long long)items_.size();
    const long long target = (long long)position_ + delta;
    if (shuffled_ && target >= count && count > 1) {
        const std::size_t last = index();
        shuffleAll();
        if (order_[0] == last) {
            std::uniform_int_distribution<std::size_t> pick(1, order_.size() - 1);
            std::swap(order_[0], order_[pick(random_engine())]);
        }
    }
    long long wrapped = target % count;
    if (wrapped < 0) wrapped += count;
    position_ = (std::size_t)wrapped;
    return true;
}

std::size_t Playlist::neighbour(int delta) const {
    const long long count = (long long)items_.size();
    long long target = ((long long)position_ + delta) % count;
    if (target < 0) target += count;
    return order_[(std::size_t)target];
}

void Playlist::select(std::size_t index) {
    if (index >= items_.size()) index = 0;
    if (shuffled_) {
        position_ = 0;
        std::swap(order_[0], *std::find(order_.begin(), order_.end(), index));
        std::shuffle(order_.begin() + 1, order_.end(), random_engine());
        order_version_++;
    } else {
        position_ = index;
    }
}

void Playlist::setShuffled(bool shuffled) {
    if (shuffled == shuffled_) return;
    const std::size_t current = index();
    shuffled_ = shuffled;
    if (!shuffled) {
        std::iota(order_.begin(), order_.end(), 0);
        order_version_++;
    }
    select(current);
}

void Playlist::shuffleAll() {
    shuffled_ = true;
    std::shuffle(order_.begin(), order_.end(), random_engine());
    order_version_++;
    position_ = 0;
}

static std::string name_of(const std::string &path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

static std::string trim(const std::string &text) {
    const std::size_t first = text.find_first_not_of(" \t\r");
    if (first == std::string::npos) return {};
    return text.substr(first, text.find_last_not_of(" \t\r") - first + 1);
}

static bool read_file(const std::string &path, std::string *text) {
    FILE *file = fopen(path.c_str(), "rb");
    if (!file) {
        ESP_LOGE(TAG, "cannot open %s", path.c_str());
        return false;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    if (size < 0 || size > kMaxPlaylistBytes) {
        ESP_LOGE(TAG, "%s: %ld bytes, at most %ld are read", path.c_str(), size, kMaxPlaylistBytes);
        fclose(file);
        return false;
    }
    text->resize((std::size_t)size);
    const std::size_t got = fread(text->data(), 1, text->size(), file);
    fclose(file);
    if (got != text->size()) {
        ESP_LOGE(TAG, "%s: read %u of %ld bytes", path.c_str(), (unsigned)got, size);
        return false;
    }
    return true;
}

/* Empty when the entry is not a file on the playlist's own storage. */
static std::string resolve_entry(std::string entry, const std::string &directory,
                                 const std::string &mount_point) {
    std::replace(entry.begin(), entry.end(), '\\', '/');
    /* No FAT name holds a ':', so one means a drive letter or a URL. */
    if (entry.find(':') != std::string::npos) return {};
    const std::string joined =
        entry[0] == '/' ? entry : directory.substr(mount_point.size()) + "/" + entry;

    std::vector<std::string> parts;
    std::size_t start = 0;
    while (start <= joined.size()) {
        std::size_t end = joined.find('/', start);
        if (end == std::string::npos) end = joined.size();
        const std::string part = joined.substr(start, end - start);
        start = end + 1;
        if (part.empty() || part == ".") continue;
        if (part == "..") {
            if (parts.empty()) return {};
            parts.pop_back();
            continue;
        }
        parts.push_back(part);
    }
    if (parts.empty()) return {};
    std::string path = mount_point;
    for (const std::string &part : parts) path += "/" + part;
    return path;
}

bool playlist_file_read(const std::string &path, PlaylistFile *file) {
    *file = {};
    const std::string name = name_of(path);
    const std::size_t dot = name.rfind('.');
    file->title = dot == 0 || dot == std::string::npos ? name : name.substr(0, dot);

    const char *mount_point = nullptr;
    for (const char *candidate : { kSdMountPoint, kUsbMountPoint }) {
        if (path_is_under(path, candidate)) mount_point = candidate;
    }
    std::string text;
    if (!mount_point || !read_file(path, &text)) return false;
    if (text.compare(0, 3, "\xef\xbb\xbf") == 0) text.erase(0, 3);

    const std::string directory = path.substr(0, path.rfind('/'));
    std::size_t start = 0;
    while (start < text.size()) {
        std::size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        const std::string line = trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty()) continue;
        if (line[0] == '#') {
            static constexpr const char kTitle[] = "#PLAYLIST:";
            if (line.compare(0, sizeof(kTitle) - 1, kTitle) != 0) continue;
            const std::string title = trim(line.substr(sizeof(kTitle) - 1));
            if (!title.empty()) file->title = title;
            continue;
        }
        const std::string entry = resolve_entry(line, directory, mount_point);
        if (entry.empty()) continue;
        std::string entry_name = name_of(entry);
        const MediaKind kind = demuxer_media_kind(entry_name.c_str());
        if (kind == MediaKind::None || kind == MediaKind::Playlist) continue;
        if (file->kind == MediaKind::None) file->kind = kind;
        if (kind != file->kind) continue;
        file->items.push_back({ std::move(entry_name), entry });
    }
    return true;
}

std::vector<PlaylistItem> playlist_items_at(const std::string &path, MediaKind kind) {
    std::vector<PlaylistItem> items;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return items;
    if (!S_ISDIR(st.st_mode)) {
        const std::string name = name_of(path);
        if (demuxer_media_kind(name.c_str()) == MediaKind::Playlist) {
            PlaylistFile file;
            if (playlist_file_read(path, &file) && file.kind == kind) items = std::move(file.items);
            return items;
        }
        if (demuxer_media_kind(name.c_str()) == kind) items.push_back({ name, path });
        return items;
    }

    PsramVector<DirectoryEntry> entries;
    if (!media_directory_list(path, &entries)) return items;
    for (const DirectoryEntry &entry : entries) {
        if (entry.directory || entry.kind != kind) continue;
        items.push_back({ entry.name.c_str(), path + "/" + entry.name.c_str() });
    }
    return items;
}
