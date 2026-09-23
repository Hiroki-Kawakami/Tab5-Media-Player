/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "playlist.hpp"
#include "media/media_directory.hpp"
#include "esp_timer.h"

#include <algorithm>
#include <numeric>
#include <random>
#include <sys/stat.h>

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
        std::shuffle(order_.begin(), order_.end(), random_engine());
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
    } else {
        position_ = index;
    }
}

void Playlist::setShuffled(bool shuffled) {
    if (shuffled == shuffled_) return;
    const std::size_t current = index();
    shuffled_ = shuffled;
    if (!shuffled) std::iota(order_.begin(), order_.end(), 0);
    select(current);
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
