/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#include "playlist.hpp"

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
