/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
#include "media/demuxer.hpp"
#include <cstddef>
#include <string>
#include <vector>

enum class RepeatMode { Off, All, One };

struct PlaylistItem {
    std::string name;
    std::string path;
};

class Playlist {
public:
    Playlist(std::vector<PlaylistItem> items, std::size_t index);

    const PlaylistItem &current() const { return items_[index()]; }
    const PlaylistItem &at(std::size_t index) const { return items_[index]; }
    std::size_t size() const { return items_.size(); }
    std::size_t index() const { return order_[position_]; }

    bool canStep(int delta, RepeatMode repeat) const;
    bool step(int delta, RepeatMode repeat);
    /* The item `delta` steps away in play order, wrapping around. */
    std::size_t neighbour(int delta) const;
    void select(std::size_t index);

    bool shuffled() const { return shuffled_; }
    void setShuffled(bool shuffled);
    /* Shuffles every item, the current one included, and moves to the first. */
    void shuffleAll();

private:
    std::vector<PlaylistItem> items_;
    std::vector<std::size_t> order_;
    std::size_t position_ = 0;
    bool shuffled_ = false;
};

/* The files of `kind` in a directory, by name, or the path itself when it is
   a file of that kind. */
std::vector<PlaylistItem> playlist_items_at(const std::string &path, MediaKind kind);
