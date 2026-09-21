/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Hiroki Kawakami
 */

#pragma once
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

    const PlaylistItem &current() const { return items_[index_]; }
    std::size_t size() const { return items_.size(); }
    std::size_t index() const { return index_; }

    bool canStep(int delta, RepeatMode repeat) const;
    bool step(int delta, RepeatMode repeat);

private:
    std::vector<PlaylistItem> items_;
    std::size_t index_ = 0;
};
