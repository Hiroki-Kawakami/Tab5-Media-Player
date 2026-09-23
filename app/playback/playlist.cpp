/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "playlist.hpp"
#include "media/media_directory.hpp"

#include <sys/stat.h>

Playlist::Playlist(std::vector<PlaylistItem> items, std::size_t index)
    : items_(std::move(items)), index_(index) {
    if (items_.empty()) items_.emplace_back();
    if (index_ >= items_.size()) index_ = 0;
}

bool Playlist::canStep(int delta, RepeatMode repeat) const {
    if (delta == 0) return false;
    if (repeat == RepeatMode::All) return true;
    const long long target = (long long)index_ + delta;
    return target >= 0 && target < (long long)items_.size();
}

bool Playlist::step(int delta, RepeatMode repeat) {
    if (!canStep(delta, repeat)) return false;
    const long long count = (long long)items_.size();
    long long target = ((long long)index_ + delta) % count;
    if (target < 0) target += count;
    index_ = (std::size_t)target;
    return true;
}

void Playlist::select(std::size_t index) {
    if (index < items_.size()) index_ = index;
}

std::vector<PlaylistItem> playlist_items_at(const std::string &path, MediaKind kind) {
    std::vector<PlaylistItem> items;
    struct stat st;
    if (stat(path.c_str(), &st) != 0) return items;
    if (!S_ISDIR(st.st_mode)) {
        const std::size_t slash = path.rfind('/');
        const std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
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
